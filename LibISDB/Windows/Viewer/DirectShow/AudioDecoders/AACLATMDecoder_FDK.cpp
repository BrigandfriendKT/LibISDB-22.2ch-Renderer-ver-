/*
  LibISDB
  Copyright(c) 2017-2020 DBCTRADO

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/**
 @file   AACLATMDecoder_FDK.cpp
 @brief  AAC-LATM(LOAS) デコーダ (FDK AAC)
 @author DBCTRADO
*/


#include "../../../../LibISDBPrivate.hpp"


#ifdef LIBISDB_HAS_FDK_AAC


#include "AACLATMDecoder_FDK.hpp"
#include "../../../../../Thirdparty/fdk-aac/libAACdec/include/aacdecoder_lib.h"
#include "../../../../Base/DebugDef.hpp"
#include <vector>


#pragma comment(lib, "fdk-aac.lib")


namespace LibISDB::DirectShow
{


AACLATMDecoder_FDK::AACLATMDecoder_FDK() noexcept
	: m_hDecoder(nullptr)
	, m_LastChannelCount(-1)
	, m_DecodeError(false)
{
}


AACLATMDecoder_FDK::~AACLATMDecoder_FDK()
{
	Close();
}


bool AACLATMDecoder_FDK::Open()
{
	if (!OpenDecoder())
		return false;

	// FDK-AACの内部出力バッファ。22.2ch(24ch)まで受けられるようにする
	constexpr size_t PCM_BUFFER_SIZE = 24 * sizeof(int16_t) * 4096;
	if (m_PCMBuffer.AllocateBuffer(PCM_BUFFER_SIZE) < PCM_BUFFER_SIZE) {
		Close();
		return false;
	}

	// 7.1ch(8ch)ダウンミックス結果用バッファ
	constexpr size_t DOWNMIX_BUFFER_SIZE = 8 * sizeof(int16_t) * 4096;
	if (m_DownmixBuffer.AllocateBuffer(DOWNMIX_BUFFER_SIZE) < DOWNMIX_BUFFER_SIZE) {
		Close();
		return false;
	}

	ClearAudioInfo();

	return true;
}


void AACLATMDecoder_FDK::Close()
{
	CloseDecoder();

	m_PCMBuffer.FreeBuffer();
	m_DownmixBuffer.FreeBuffer();
}


bool AACLATMDecoder_FDK::IsOpened() const
{
	return m_hDecoder != nullptr;
}


bool AACLATMDecoder_FDK::Reset()
{
	if (!ResetDecoder())
		return false;

	ClearAudioInfo();
	m_DecodeError = false;

	return true;
}


bool AACLATMDecoder_FDK::GetChannelMap(int Channels, int *pMap) const
{
	switch (Channels) {
	case 2:
		pMap[CHANNEL_2_L] = 0;
		pMap[CHANNEL_2_R] = 1;
		break;

	case 6:
		pMap[CHANNEL_6_FL]  = 0;
		pMap[CHANNEL_6_FR]  = 1;
		pMap[CHANNEL_6_FC]  = 2;
		pMap[CHANNEL_6_LFE] = 3;
		pMap[CHANNEL_6_BL]  = 4;
		pMap[CHANNEL_6_BR]  = 5;
		break;

	case 8:
		// DownmixTo7_1()の出力順(FL,FR,FC,LFE,BL,BR,SL,SR)と対応させる
		pMap[CHANNEL_8_FL]  = 0;
		pMap[CHANNEL_8_FR]  = 1;
		pMap[CHANNEL_8_FC]  = 2;
		pMap[CHANNEL_8_LFE] = 3;
		pMap[CHANNEL_8_BL]  = 4;
		pMap[CHANNEL_8_BR]  = 5;
		pMap[CHANNEL_8_SL]  = 6;
		pMap[CHANNEL_8_SR]  = 7;
		break;

	default:
		return false;
	}

	return true;
}


bool AACLATMDecoder_FDK::GetDownmixInfo(ReturnArg<DownmixInfo> Info) const
{
	if (!Info)
		return false;

	// AACDecoder_FDK と同じダウンミックス設定(STD-B21 6.2.1)を使用する
	constexpr double PSQR = 1.0 / 1.4142135623730950488016887242097;

	Info->Center = PSQR;
	Info->Front  = 1.0;
	Info->Rear   = PSQR;
	Info->LFE    = 0.0;

	return true;
}


bool AACLATMDecoder_FDK::OpenDecoder()
{
	CloseDecoder();

	// FDK AAC オープン(LOAS/LATMトランスポート)
	// stream_type 0x11 (ISO/IEC 14496-3 LATM) は LOAS フレームで
	// 自己同期可能な形で伝送されるため、TT_MP4_LOAS を指定する。
	// LOAS の同期・AudioMuxElement・StreamMuxConfig・PayloadLengthInfo の
	// 解析はすべて FDK-AAC 内部で行われるため、LibISDB側での
	// LOASパーサーの実装は不要となる。
	m_hDecoder = ::aacDecoder_Open(TT_MP4_LOAS, 1);
	if (m_hDecoder == nullptr) {
		LIBISDB_TRACE(LIBISDB_STR("AACLATMDecoder_FDK::OpenDecoder() aacDecoder_Open() failed\n"));
		return false;
	}
	LIBISDB_TRACE(LIBISDB_STR("AACLATMDecoder_FDK::OpenDecoder() aacDecoder_Open() OK\n"));

	// デコーダのパラメータを設定
	// MAX/MIN_OUTPUT_CHANNELSは設定しない(=-1相当)。
	// ストリームが持つ本来のチャンネル数(2ch/6ch/22.2ch=24ch等)のまま
	// 受け取り、8chを超える場合の変換はDownmixTo7_1()で自前で行う。
	// (このパラメータは8ch超の入力には元々効かない仕様のため)
	::aacDecoder_SetParam(m_hDecoder, AAC_PCM_OUTPUT_CHANNEL_MAPPING, 1);
	::aacDecoder_SetParam(m_hDecoder, AAC_PCM_LIMITER_ENABLE, 0);

	m_LastChannelCount = -1;
	m_DecodeError = false;

	return true;
}


void AACLATMDecoder_FDK::CloseDecoder()
{
	// FDK AAC クローズ
	if (m_hDecoder != nullptr) {
		::aacDecoder_Close(m_hDecoder);
		m_hDecoder = nullptr;
	}
}


bool AACLATMDecoder_FDK::ResetDecoder()
{
	if (!IsOpened())
		return false;

	return OpenDecoder();
}


namespace
{

	// AUDIO_CHANNEL_TYPE のビット構造(FDK_audio.h)
	// Bit 0-3: 水平位置 (1:front 2:side 3:back 4:lfe)
	// Bit 4-7: 垂直位置 (0:通常 1:上層 2:下層)
	constexpr int ACT_HORIZ_MASK = 0x0F;
	constexpr int ACT_FRONT = 0x01;
	constexpr int ACT_SIDE  = 0x02;
	constexpr int ACT_BACK  = 0x03;
	constexpr int ACT_LFE   = 0x04;
	constexpr int ACT_VERT_MASK = 0xF0;
	constexpr int ACT_NORMAL_V = 0x00;

	// 出力チャンネル(7.1ch: FL,FR,FC,LFE,BL,BR,SL,SR)への加算先とゲイン
	// (振り分けはEstimatePosition()とswitch文で直接行う)

	// 入力チャンネルが左寄り/中央/右寄りのどれかを、そのグループ内での
	// 出現順(pChannelIndices)とグループ内チャンネル数から大まかに判定する
	enum class Position { Left, Center, Right };

	Position EstimatePosition(uint8_t IndexInGroup, int GroupCount)
	{
		if (GroupCount <= 1)
			return Position::Center;
		if (IndexInGroup == 0)
			return Position::Left;
		if (IndexInGroup == 1)
			return Position::Right;
		// 3個目以降(ワイドチャンネル等)はL/Rへ均等に振り分ける
		return (IndexInGroup % 2) == 0 ? Position::Left : Position::Right;
	}

}	// namespace


void AACLATMDecoder_FDK::DownmixTo7_1(
	const int16_t *pSrc, int SrcChannels,
	const int *pChannelType, const uint8_t *pChannelIndices,
	int16_t *pDst, int FrameSize)
{
	// 出力チャンネル順: FL,FR,FC,LFE,BL,BR,SL,SR
	constexpr int OUT_FL = 0, OUT_FR = 1, OUT_FC = 2, OUT_LFE = 3;
	constexpr int OUT_BL = 4, OUT_BR = 5, OUT_SL = 6, OUT_SR = 7;
	constexpr int OUT_CHANNELS = 8;

	// 各horizontal種別内でのチャンネル数を数える(L/R振り分けの精度を上げるため)
	int GroupCount[5] = {}; // index: ACT_FRONT/SIDE/BACK/LFE (ACT_NONE分は未使用)
	for (int ch = 0; ch < SrcChannels; ch++) {
		const int Horiz = pChannelType[ch] & ACT_HORIZ_MASK;
		if ((Horiz >= ACT_FRONT) && (Horiz <= ACT_LFE))
			GroupCount[Horiz]++;
	}

	std::vector<float> Mix(static_cast<size_t>(OUT_CHANNELS) * FrameSize, 0.0f);

	for (int ch = 0; ch < SrcChannels; ch++) {
		const int Horiz = pChannelType[ch] & ACT_HORIZ_MASK;
		const int Vert  = pChannelType[ch] & ACT_VERT_MASK;
		// 上層/下層チャンネルは通常層へ-3dBで折り込む
		const float HeightGain = (Vert == ACT_NORMAL_V) ? 1.0f : 0.708f;

		int OutIndexL = -1, OutIndexR = -1, OutIndexC = -1;
		bool ToLFE = false;

		switch (Horiz) {
		case ACT_FRONT: OutIndexL = OUT_FL; OutIndexR = OUT_FR; OutIndexC = OUT_FC; break;
		case ACT_SIDE:  OutIndexL = OUT_SL; OutIndexR = OUT_SR; OutIndexC = -1;     break;
		case ACT_BACK:  OutIndexL = OUT_BL; OutIndexR = OUT_BR; OutIndexC = -1;     break;
		case ACT_LFE:   ToLFE = true; break;
		default: continue; // 未知の種別は無視
		}

		if (ToLFE) {
			for (int i = 0; i < FrameSize; i++)
				Mix[static_cast<size_t>(i) * OUT_CHANNELS + OUT_LFE] += pSrc[static_cast<size_t>(i) * SrcChannels + ch];
			continue;
		}

		const int GroupN = GroupCount[Horiz];
		const Position Pos = EstimatePosition(pChannelIndices[ch], GroupN);

		int OutIndex;
		float Gain = HeightGain;
		if ((Pos == Position::Center) && (OutIndexC >= 0)) {
			OutIndex = OutIndexC;
		} else if (Pos == Position::Left) {
			OutIndex = OutIndexL;
		} else if (Pos == Position::Right) {
			OutIndex = OutIndexR;
		} else {
			// side/backにセンターは無いのでL/Rへ均等に混ぜる
			for (int i = 0; i < FrameSize; i++) {
				const float S = pSrc[static_cast<size_t>(i) * SrcChannels + ch] * Gain * 0.708f;
				Mix[static_cast<size_t>(i) * OUT_CHANNELS + OutIndexL] += S;
				Mix[static_cast<size_t>(i) * OUT_CHANNELS + OutIndexR] += S;
			}
			continue;
		}

		for (int i = 0; i < FrameSize; i++) {
			Mix[static_cast<size_t>(i) * OUT_CHANNELS + OutIndex] +=
				pSrc[static_cast<size_t>(i) * SrcChannels + ch] * Gain;
		}
	}

	// クリッピング防止のためのヘッドルーム(22.2ch由来の多重加算を考慮し-6dB)
	constexpr float OutputScale = 0.5f;

	for (size_t i = 0; i < Mix.size(); i++) {
		float v = Mix[i] * OutputScale;
		if (v > 32767.0f)
			v = 32767.0f;
		else if (v < -32768.0f)
			v = -32768.0f;
		pDst[i] = static_cast<int16_t>(v);
	}
}


bool AACLATMDecoder_FDK::Decode(const uint8_t *pData, size_t *pDataSize, ReturnArg<DecodeFrameInfo> Info)
{
	if (!IsOpened() || (pData == nullptr) || (pDataSize == nullptr))
		return false;

	Info->SampleCount = 0;

	UCHAR *p = const_cast<UCHAR *>(pData);
	UINT Avail = static_cast<UINT>(*pDataSize);
	const UINT InputSize = Avail;

	if (InputSize == 0) {
		*pDataSize = 0;
		return true;
	}

	// PESペイロードの生バイト列をそのままFDK-AACの内部バッファへ投入する。
	// LOASの同期処理・複数フレームの分離は全てFDK-AAC側が行う。
	const UINT Size = Avail;
	AAC_DECODER_ERROR Err = ::aacDecoder_Fill(m_hDecoder, &p, &Size, &Avail);
	if (Err != AAC_DEC_OK) {
		LIBISDB_TRACE(
			LIBISDB_STR("aacDecoder_Fill() error {:#X}\n"),
			static_cast<std::underlying_type_t<AAC_DECODER_ERROR>>(Err));
		// 同期エラー等。今回投入した分は破棄して先へ進める
		*pDataSize = InputSize;
		m_DecodeError = true;
		return true;
	}

	// Fillが取り込んだバイト数を消費済みとして呼び出し元へ返す
	*pDataSize = InputSize - Avail;
	if (*pDataSize == 0) {
		// FDK-AAC内部バッファが飽和している等、通常は起こらないケース。
		// 呼び出し側の無限ループを避けるため、投入分は消費済み扱いにする。
		LIBISDB_TRACE(LIBISDB_STR("AACLATMDecoder_FDK::Decode() aacDecoder_Fill() consumed no data\n"));
		*pDataSize = InputSize;
	}

	AAC_DECODER_ERROR DecErr = ::aacDecoder_DecodeFrame(
		m_hDecoder,
		reinterpret_cast<INT_PCM *>(m_PCMBuffer.GetBuffer()),
		static_cast<INT>(m_PCMBuffer.GetBufferSize() / sizeof(int16_t)),
		m_DecodeError ? AACDEC_INTR : 0);

	if ((DecErr == AAC_DEC_TRANSPORT_SYNC_ERROR) || (DecErr == AAC_DEC_NOT_ENOUGH_BITS)) {
		// まだ1フレーム分のデータが揃っていない。次のデータを待つ
		// (原因切り分けのため、頻発していないか分かるようにログを出す)
		LIBISDB_TRACE(
			LIBISDB_STR("AACLATMDecoder_FDK::Decode() aacDecoder_DecodeFrame() {} (Consumed={}/{})\n"),
			(DecErr == AAC_DEC_TRANSPORT_SYNC_ERROR) ?
				LIBISDB_STR("AAC_DEC_TRANSPORT_SYNC_ERROR") : LIBISDB_STR("AAC_DEC_NOT_ENOUGH_BITS"),
			*pDataSize, InputSize);
		return true;
	}

	if (DecErr != AAC_DEC_OK) {
		LIBISDB_TRACE(
			LIBISDB_STR("aacDecoder_DecodeFrame() error {:#X}\n"),
			static_cast<std::underlying_type_t<AAC_DECODER_ERROR>>(DecErr));
		m_DecodeError = true;
		return true;
	}

	const ::CStreamInfo *pStreamInfo = ::aacDecoder_GetStreamInfo(m_hDecoder);

	if (pStreamInfo->numChannels != m_LastChannelCount) {
		LIBISDB_TRACE(
			LIBISDB_STR("AACLATMDecoder_FDK::Decode() Channel count changed {} -> {}\n"),
			m_LastChannelCount, pStreamInfo->numChannels);
		m_LastChannelCount = pStreamInfo->numChannels;
	}

	if (pStreamInfo->numChannels > 8) {
		// 22.2ch等、8chを超える場合は自前で7.1chへダウンミックスする
		// (FDK-AACのAAC_PCM_MAX_OUTPUT_CHANNELSは8ch超の入力には効かないため)
		DownmixTo7_1(
			reinterpret_cast<const int16_t *>(m_PCMBuffer.GetBuffer()),
			pStreamInfo->numChannels,
			reinterpret_cast<const int *>(pStreamInfo->pChannelType),
			pStreamInfo->pChannelIndices,
			reinterpret_cast<int16_t *>(m_DownmixBuffer.GetBuffer()),
			pStreamInfo->frameSize);

		m_AudioInfo.Frequency = pStreamInfo->sampleRate;
		m_AudioInfo.ChannelCount = 8;
		m_AudioInfo.OriginalChannelCount = pStreamInfo->numChannels;
		m_AudioInfo.DualMono = false;

		Info->pData = m_DownmixBuffer.GetBuffer();
	} else {
		m_AudioInfo.Frequency = pStreamInfo->sampleRate;
		m_AudioInfo.ChannelCount = pStreamInfo->numChannels;
		m_AudioInfo.OriginalChannelCount = pStreamInfo->numChannels;
		// LATMでのデュアルモノ判定はチャンネル構成の追加調査が必要なため、
		// 現状は非対応(通常のステレオ/5.1chとして扱う)
		m_AudioInfo.DualMono = false;

		Info->pData = m_PCMBuffer.GetBuffer();
	}

	Info->SampleCount = pStreamInfo->frameSize;
	Info->Info = m_AudioInfo;
	Info->Discontinuity = m_DecodeError;

	m_DecodeError = false;

	return true;
}


#endif // LIBISDB_HAS_FDK_AAC


} // namespace LibISDB::DirectShow
