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
 @file   AACLATMDecoder_Libav.cpp
 @brief  AAC-LATM(LOAS) デコーダ (libavcodec)
 @author DBCTRADO
*/


#include "../../../../LibISDBPrivate.hpp"


#ifdef LIBISDB_HAS_LIBAV_AAC


#include "../../../../LibISDBWindows.hpp"
#include "AACLATMDecoder_Libav.hpp"
#include "LatmDownmixMatrix.hpp"
#include "LatmSpatialChannelMap.hpp"
#include "../../../../Base/DebugDef.hpp"
#include <vector>
#include <cstdio>
#include <cwchar>
#include <iterator>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}


#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avutil.lib")


namespace LibISDB::DirectShow
{


namespace
{

	// 出力チャンネル順
	// 7.1ch(8ch): FL,FR,FC,LFE,BL,BR,SL,SR
	// 5.1ch(6ch): FL,FR,FC,LFE,BL,BR (SL,SRは存在しない)
	constexpr int OUT_FL = 0, OUT_FR = 1, OUT_FC = 2, OUT_LFE = 3;
	constexpr int OUT_BL = 4, OUT_BR = 5, OUT_SL = 6, OUT_SR = 7;

	constexpr float HEIGHT_GAIN = 0.708f; // 上層/下層チャンネルを通常層へ折り込む際のゲイン(-3dB)
	constexpr float SPLIT_GAIN  = 0.5f;   // 中央寄りのチャンネルをL/R双方へ振り分ける際のゲイン

	// AVChannelを出力チャンネルへ分類する(最大2箇所へ振り分け可能)
	// TargetChannelsが6の場合、side/top-side系はBL/BRへ統合する(5.1chにはS(ide)が無いため)
	// 戻り値: 振り分け先の数(0=非対応チャンネルにつき無視)
	int ClassifyChannel(int Channel, int TargetChannels, int *pBucket1, float *pGain1, int *pBucket2, float *pGain2)
	{
		const int SideL = (TargetChannels >= 8) ? OUT_SL : OUT_BL;
		const int SideR = (TargetChannels >= 8) ? OUT_SR : OUT_BR;
		// 5.1chへ統合する場合、BLに元々あるリアと重複しすぎないよう若干減衰させる
		const float SideGain = (TargetChannels >= 8) ? 1.0f : HEIGHT_GAIN;
		const float SideHeightGain = (TargetChannels >= 8) ? HEIGHT_GAIN : (HEIGHT_GAIN * HEIGHT_GAIN);

		switch (Channel) {
		// 通常層(主要チャンネル)
		case AV_CHAN_FRONT_LEFT:           *pBucket1 = OUT_FL;  *pGain1 = 1.0f; return 1;
		case AV_CHAN_FRONT_RIGHT:          *pBucket1 = OUT_FR;  *pGain1 = 1.0f; return 1;
		case AV_CHAN_FRONT_CENTER:         *pBucket1 = OUT_FC;  *pGain1 = 1.0f; return 1;
		case AV_CHAN_LOW_FREQUENCY:        *pBucket1 = OUT_LFE; *pGain1 = 1.0f; return 1;
		case AV_CHAN_LOW_FREQUENCY_2:      *pBucket1 = OUT_LFE; *pGain1 = 1.0f; return 1;
		case AV_CHAN_BACK_LEFT:            *pBucket1 = OUT_BL;  *pGain1 = 1.0f; return 1;
		case AV_CHAN_BACK_RIGHT:           *pBucket1 = OUT_BR;  *pGain1 = 1.0f; return 1;
		case AV_CHAN_SIDE_LEFT:            *pBucket1 = SideL;   *pGain1 = SideGain; return 1;
		case AV_CHAN_SIDE_RIGHT:           *pBucket1 = SideR;   *pGain1 = SideGain; return 1;

		// ワイドチャンネル(FC寄りのL/R)
		case AV_CHAN_FRONT_LEFT_OF_CENTER:  *pBucket1 = OUT_FL; *pGain1 = HEIGHT_GAIN; return 1;
		case AV_CHAN_FRONT_RIGHT_OF_CENTER: *pBucket1 = OUT_FR; *pGain1 = HEIGHT_GAIN; return 1;

		// バックセンター(L/Rへ均等に振り分け)
		case AV_CHAN_BACK_CENTER:
			*pBucket1 = OUT_BL; *pGain1 = SPLIT_GAIN;
			*pBucket2 = OUT_BR; *pGain2 = SPLIT_GAIN;
			return 2;

		// 上層(通常層へ-3dBで折り込む)
		case AV_CHAN_TOP_FRONT_LEFT:   *pBucket1 = OUT_FL;  *pGain1 = HEIGHT_GAIN; return 1;
		case AV_CHAN_TOP_FRONT_CENTER: *pBucket1 = OUT_FC;  *pGain1 = HEIGHT_GAIN; return 1;
		case AV_CHAN_TOP_FRONT_RIGHT:  *pBucket1 = OUT_FR;  *pGain1 = HEIGHT_GAIN; return 1;
		case AV_CHAN_TOP_BACK_LEFT:    *pBucket1 = OUT_BL;  *pGain1 = HEIGHT_GAIN; return 1;
		case AV_CHAN_TOP_BACK_RIGHT:   *pBucket1 = OUT_BR;  *pGain1 = HEIGHT_GAIN; return 1;
		case AV_CHAN_TOP_SIDE_LEFT:    *pBucket1 = SideL;   *pGain1 = SideHeightGain; return 1;
		case AV_CHAN_TOP_SIDE_RIGHT:   *pBucket1 = SideR;   *pGain1 = SideHeightGain; return 1;
		case AV_CHAN_TOP_CENTER:       *pBucket1 = OUT_FC;  *pGain1 = SPLIT_GAIN;  return 1;
		case AV_CHAN_TOP_BACK_CENTER:
			*pBucket1 = OUT_BL; *pGain1 = SPLIT_GAIN;
			*pBucket2 = OUT_BR; *pGain2 = SPLIT_GAIN;
			return 2;

		// 下層(通常層へ-3dBで折り込む)
		case AV_CHAN_BOTTOM_FRONT_LEFT:   *pBucket1 = OUT_FL; *pGain1 = HEIGHT_GAIN; return 1;
		case AV_CHAN_BOTTOM_FRONT_CENTER: *pBucket1 = OUT_FC; *pGain1 = HEIGHT_GAIN; return 1;
		case AV_CHAN_BOTTOM_FRONT_RIGHT:  *pBucket1 = OUT_FR; *pGain1 = HEIGHT_GAIN; return 1;

		default:
			// 未知のチャンネル種別は無視する
			return 0;
		}
	}

}	// namespace


AACLATMDecoder_Libav::AACLATMDecoder_Libav() noexcept
	: m_pCodecContext(nullptr)
	, m_pParser(nullptr)
	, m_pPacket(nullptr)
	, m_pFrame(nullptr)
	, m_LastChannelCount(-1)
	, m_DecodeError(false)
	, m_DownmixChannels(8)
	, m_MatrixValid(false)
{
	ResolveDownmixMatrix();
}


AACLATMDecoder_Libav::~AACLATMDecoder_Libav()
{
	Close();
}


void AACLATMDecoder_Libav::SetDownmixChannels(int Channels)
{
	// 8(7.1ch) / 6(5.1ch) / 2(2ch) / 0(ダウンミックスしない) のみ有効
	if ((Channels == 8) || (Channels == 6) || (Channels == 2) || (Channels == 0)) {
		m_DownmixChannels = Channels;
		ResolveDownmixMatrix();
	}
}


void AACLATMDecoder_Libav::SetDownmixPresetName(const LibISDB::CharType *pName)
{
	m_PresetNameOverride = (pName != nullptr) ? pName : LIBISDB_STR("");
	ResolveDownmixMatrix();
}


void AACLATMDecoder_Libav::ResolveDownmixMatrix()
{
	m_MatrixValid = false;

	if (m_DownmixChannels == 0) {
		// 「ダウンミックスしない」設定。この場合はConvertFrameRaw24ch()で
		// 22.2ch(24ch)を固定順序へ並び替えてそのまま出力するため、
		// マトリクスの解決は不要
		return;
	}

	// 明示的なプリセット名が指定されていればそれを、無ければ
	// ダウンミックス先ch数から決まる既定名("7.1ch"/"5.1ch"/"2ch")を使う
	const LibISDB::CharType *pPresetName =
		!m_PresetNameOverride.empty() ? m_PresetNameOverride.c_str() :
		(m_DownmixChannels == 8) ? LIBISDB_STR("7.1ch") :
		(m_DownmixChannels == 6) ? LIBISDB_STR("5.1ch") : LIBISDB_STR("2ch");

	LibISDB::CharType szIniPath[MAX_PATH];
	LatmDownmix::Matrix Matrix;
	bool Resolved;

	if (LatmDownmix::GetDefaultPresetFileName(szIniPath, std::size(szIniPath))) {
		Resolved = LatmDownmix::ResolvePreset(szIniPath, pPresetName, &Matrix);
	} else {
		// iniのパスが取得できなかった場合(通常起こらない)は組み込み定義のみ試す
		Resolved = LatmDownmix::GetBuiltinPreset(pPresetName, &Matrix);
	}

	if (Resolved) {
		m_Matrix = std::make_unique<LatmDownmix::Matrix>(std::move(Matrix));
		m_MatrixValid = true;
	} else {
		// 組み込み定義まで含めて解決できなかった場合(iniの当該プリセットの
		// 記述が不正な場合等)。安全のため無音とする
		LIBISDB_TRACE(
			LIBISDB_STR("AACLATMDecoder_Libav::ResolveDownmixMatrix() プリセット '{}' の解決に失敗しました\n"),
			pPresetName);
	}
}


bool AACLATMDecoder_Libav::Open()
{
	if (!OpenDecoder())
		return false;

	// 出力バッファ(7.1ch分で確保。8ch以下やダウンミックス後もこれで足りる)
	constexpr size_t PCM_BUFFER_SIZE = 24 * sizeof(int16_t) * 4096; // 22.2ch(24ch)まで対応
	if (m_PCMBuffer.AllocateBuffer(PCM_BUFFER_SIZE) < PCM_BUFFER_SIZE) {
		Close();
		return false;
	}

	ClearAudioInfo();

	return true;
}


void AACLATMDecoder_Libav::Close()
{
	CloseDecoder();

	m_PCMBuffer.FreeBuffer();
}


bool AACLATMDecoder_Libav::IsOpened() const
{
	return m_pCodecContext != nullptr;
}


bool AACLATMDecoder_Libav::Reset()
{
	if (!ResetDecoder())
		return false;

	ClearAudioInfo();
	m_DecodeError = false;

	return true;
}


bool AACLATMDecoder_Libav::GetChannelMap(int Channels, int *pMap) const
{
	// ConvertFrame()が常に標準順序(FL,FR,FC,LFE,BL,BR,SL,SR)へ詰めて
	// 出力するため、恒等写像でよい
	switch (Channels) {
	case 2:
		pMap[CHANNEL_2_L] = 0;
		pMap[CHANNEL_2_R] = 1;
		return true;

	case 6:
		pMap[CHANNEL_6_FL]  = 0;
		pMap[CHANNEL_6_FR]  = 1;
		pMap[CHANNEL_6_FC]  = 2;
		pMap[CHANNEL_6_LFE] = 3;
		pMap[CHANNEL_6_BL]  = 4;
		pMap[CHANNEL_6_BR]  = 5;
		return true;

	case 8:
		pMap[CHANNEL_8_FL]  = 0;
		pMap[CHANNEL_8_FR]  = 1;
		pMap[CHANNEL_8_FC]  = 2;
		pMap[CHANNEL_8_LFE] = 3;
		pMap[CHANNEL_8_BL]  = 4;
		pMap[CHANNEL_8_BR]  = 5;
		pMap[CHANNEL_8_SL]  = 6;
		pMap[CHANNEL_8_SR]  = 7;
		return true;
	}

	return false;
}


bool AACLATMDecoder_Libav::GetDownmixInfo(ReturnArg<DownmixInfo> Info) const
{
	if (!Info)
		return false;

	constexpr double PSQR = 1.0 / 1.4142135623730950488016887242097;

	Info->Center = PSQR;
	Info->Front  = 1.0;
	Info->Rear   = PSQR;
	Info->LFE    = 0.0;

	return true;
}


bool AACLATMDecoder_Libav::GetVersion(std::string *pVersion)
{
	if (pVersion == nullptr)
		return false;

	const unsigned Ver = ::avcodec_version();
	char Buffer[64];
	std::snprintf(
		Buffer, sizeof(Buffer), "libavcodec %u.%u.%u",
		AV_VERSION_MAJOR(Ver), AV_VERSION_MINOR(Ver), AV_VERSION_MICRO(Ver));
	*pVersion = Buffer;

	return true;
}


bool AACLATMDecoder_Libav::OpenDecoder()
{
	CloseDecoder();

	const AVCodec *pCodec = ::avcodec_find_decoder_by_name("aac_latm");
	if (pCodec == nullptr) {
		LIBISDB_TRACE(LIBISDB_STR("AACLATMDecoder_Libav::OpenDecoder() aac_latm decoder not found\n"));
		return false;
	}

	m_pCodecContext = ::avcodec_alloc_context3(pCodec);
	if (m_pCodecContext == nullptr)
		return false;

	// AACのデコード自体は軽い処理でマルチスレッド化の恩恵がほぼ無く、
	// 一方でバックグラウンドスレッドが動いたままデコーダを破棄すると
	// 解放済みメモリへアクセスしうる(デコーダ切り替え時に他のデコーダの
	// 状態を破壊する不具合の原因になりうる)。そのため明示的に
	// シングルスレッドへ固定する
	m_pCodecContext->thread_count = 1;
	m_pCodecContext->thread_type = 0;

	if (::avcodec_open2(m_pCodecContext, pCodec, nullptr) < 0) {
		LIBISDB_TRACE(LIBISDB_STR("AACLATMDecoder_Libav::OpenDecoder() avcodec_open2() failed\n"));
		CloseDecoder();
		return false;
	}

	m_pParser = ::av_parser_init(pCodec->id);
	if (m_pParser == nullptr) {
		LIBISDB_TRACE(LIBISDB_STR("AACLATMDecoder_Libav::OpenDecoder() av_parser_init() failed\n"));
		CloseDecoder();
		return false;
	}

	m_pPacket = ::av_packet_alloc();
	m_pFrame = ::av_frame_alloc();
	if ((m_pPacket == nullptr) || (m_pFrame == nullptr)) {
		CloseDecoder();
		return false;
	}

	m_LastChannelCount = -1;
	m_DecodeError = false;

	LIBISDB_TRACE(LIBISDB_STR("AACLATMDecoder_Libav::OpenDecoder() OK\n"));

	return true;
}


void AACLATMDecoder_Libav::CloseDecoder()
{
	if (m_pFrame != nullptr) {
		::av_frame_free(&m_pFrame);
		m_pFrame = nullptr;
	}
	if (m_pPacket != nullptr) {
		::av_packet_free(&m_pPacket);
		m_pPacket = nullptr;
	}
	if (m_pParser != nullptr) {
		::av_parser_close(m_pParser);
		m_pParser = nullptr;
	}
	if (m_pCodecContext != nullptr) {
		::avcodec_free_context(&m_pCodecContext);
		m_pCodecContext = nullptr;
	}
}


bool AACLATMDecoder_Libav::ResetDecoder()
{
	if (!IsOpened())
		return false;

	return OpenDecoder();
}


int AACLATMDecoder_Libav::ConvertFrame(const AVFrame *pFrame, int16_t *pDst)
{
	constexpr int MAX_OUT_CHANNELS = 8;

	const int SrcChannels = pFrame->ch_layout.nb_channels;
	const int Samples = pFrame->nb_samples;
	const bool NeedDownmix = (SrcChannels > MAX_OUT_CHANNELS);

	if (m_DownmixChannels == 0) {
		// 「ダウンミックスしない」設定。22.2ch(24ch)を固定順序へ並び替えて
		// そのまま出力する(AudioDecoderFilter側が24ch出力に対応済みの
		// 前提。対応する出力デバイス/レンダラが無い場合、下流でエラーに
		// なるか無音になる)
		//
		// 元の音声が8ch以下(ステレオ/5.1ch等)の場合も、同じ24ch形式で
		// 出力する(存在しないchは無音)。出力形式を常に24chに固定して
		// おくことで、22.2chと他の音声トラックを切り替えても、
		// 空間オーディオ用レンダラが受け付けられない形式への変更
		// (=レンダラの繋ぎ直しが必要になり、失敗すると無音のまま)が
		// 発生しなくなる
		return ConvertFrameRaw24ch(pFrame, pDst);
	}

	if (NeedDownmix && !m_MatrixValid) {
		// プリセット(7.1ch/5.1ch)の解決に失敗している場合の安全策。
		// 8chを超える音声(22.2ch等)は再生しない
		return 0;
	}

	const int TargetChannels =
		NeedDownmix ? static_cast<int>(m_Matrix->OutputChannels.size()) : MAX_OUT_CHANNELS;

	// float(FLTP,チャンネルごとに独立配列)から、標準順序の
	// int16インターリーブ配列へ変換・ミックスする
	// (Mixのチャンネル方向の幅は常にMAX_OUT_CHANNELSで確保し、
	//  実際に使う先頭TargetChannels/UsedBuckets分だけを後段で読む)
	std::vector<float> Mix(static_cast<size_t>(MAX_OUT_CHANNELS) * Samples, 0.0f);
	int UsedBuckets = 0; // 実際に使用された出力チャンネル数の判定に使用

	if (NeedDownmix) {
		// LatmDownmixPresets.ini(または組み込み定義)で決まった
		// チャンネルマトリクスに従ってミックスする
		for (int Out = 0; Out < TargetChannels; Out++) {
			const LatmDownmix::OutputChannelDef &Def = m_Matrix->OutputChannels[Out];

			for (const LatmDownmix::Route &R : Def.Routes) {
				// このプリセットが参照している入力chが、実際のフレームの
				// チャンネルレイアウトに含まれているかを調べる
				int SrcIndex = -1;
				for (int ch = 0; ch < SrcChannels; ch++) {
					if (::av_channel_layout_channel_from_index(&pFrame->ch_layout, ch) == R.InputChannel) {
						SrcIndex = ch;
						break;
					}
				}
				if (SrcIndex < 0)
					continue; // 該当chが無い(放送によっては22.2ch未満のこともある)

				const float *pSrc = reinterpret_cast<const float *>(pFrame->extended_data[SrcIndex]);
				for (int i = 0; i < Samples; i++)
					Mix[static_cast<size_t>(i) * MAX_OUT_CHANNELS + Out] += pSrc[i] * R.Gain;
			}
		}

		UsedBuckets = TargetChannels;
	} else {
		// 8ch以下の入力は標準的なチャンネル構成(2ch/6ch等)であることを
		// 前提としており、L/R/FC/LFE/BL/BR/SL/SRの先頭から連続して
		// 埋まる想定(GetChannelMap()の恒等写像と対応させるため)。
		// この経路はマトリクスエンジンの対象外(従来通りClassifyChannel()を使う)
		bool BucketUsed[MAX_OUT_CHANNELS] = {};

		for (int ch = 0; ch < SrcChannels; ch++) {
			const AVChannel Channel = ::av_channel_layout_channel_from_index(&pFrame->ch_layout, ch);

			int Bucket1 = -1, Bucket2 = -1;
			float Gain1 = 0.0f, Gain2 = 0.0f;
			const int Count = ClassifyChannel(Channel, MAX_OUT_CHANNELS, &Bucket1, &Gain1, &Bucket2, &Gain2);
			if (Count == 0)
				continue;

			const float *pSrc = reinterpret_cast<const float *>(pFrame->extended_data[ch]);

			BucketUsed[Bucket1] = true;
			for (int i = 0; i < Samples; i++)
				Mix[static_cast<size_t>(i) * MAX_OUT_CHANNELS + Bucket1] += pSrc[i] * Gain1;

			if (Count == 2) {
				BucketUsed[Bucket2] = true;
				for (int i = 0; i < Samples; i++)
					Mix[static_cast<size_t>(i) * MAX_OUT_CHANNELS + Bucket2] += pSrc[i] * Gain2;
			}
		}

		for (int i = 0; i < MAX_OUT_CHANNELS; i++) {
			if (BucketUsed[i])
				UsedBuckets = i + 1;
		}
	}

	// float(-1.0～1.0) -> int16へ変換。ダウンミックス時のみ
	// 複数チャンネルの加算によるクリッピングを避けるヘッドルーム
	// (m_Matrix->OutputGain。既定0.9、iniの"Gain="行で調整可能)を掛ける
	const float Scale = (NeedDownmix ? m_Matrix->OutputGain : 1.0f) * 32767.0f;

	for (int i = 0; i < Samples; i++) {
		for (int ch = 0; ch < UsedBuckets; ch++) {
			float v = Mix[static_cast<size_t>(i) * MAX_OUT_CHANNELS + ch] * Scale;
			if (v > 32767.0f)
				v = 32767.0f;
			else if (v < -32768.0f)
				v = -32768.0f;
			pDst[static_cast<size_t>(i) * UsedBuckets + ch] = static_cast<int16_t>(v);
		}
	}

	return UsedBuckets;
}


int AACLATMDecoder_Libav::ConvertFrameRaw24ch(const AVFrame *pFrame, int16_t *pDst)
{
	const int SrcChannels = pFrame->ch_layout.nb_channels;
	const int Samples = pFrame->nb_samples;

	// 診断用: 最初の1回だけ、24chのうちどのchが実際に見つかったか/
	// 見つからなかったか(=無音になっているか)をデバッグ出力する。
	// (OutputDebugStringを直接使っているため、Release/Debugどちらの
	//  ビルドでも、DebugView等のツールやVisual Studioの「出力」ウィンドウ
	//  (デバッグ実行時)で確認できる。原因を特定したら削除して構わない)
	static bool TraceLogged = false;
	if (!TraceLogged) {
		TraceLogged = true;
		wchar_t szBuff[256];
		swprintf_s(szBuff, L"[LatmSpatial] source channel count = %d\n", SrcChannels);
		::OutputDebugStringW(szBuff);
		for (int Out = 0; Out < LatmSpatial::RAW_CHANNEL_COUNT; Out++) {
			const AVChannel Channel = LatmSpatial::RawChannelOrder[Out];
			int SrcIndex = -1;
			for (int ch = 0; ch < SrcChannels; ch++) {
				if (::av_channel_layout_channel_from_index(&pFrame->ch_layout, ch) == Channel) {
					SrcIndex = ch;
					break;
				}
			}
			const LibISDB::CharType *pName = LatmDownmix::NameFromChannel(Channel);
			swprintf_s(
				szBuff, L"[LatmSpatial] slot %2d: ch=%d (%s) -> SrcIndex=%d\n",
				Out, static_cast<int>(Channel), pName ? pName : L"?", SrcIndex);
			::OutputDebugStringW(szBuff);
		}
	}

	for (int Out = 0; Out < LatmSpatial::RAW_CHANNEL_COUNT; Out++) {
		const AVChannel Channel = LatmSpatial::RawChannelOrder[Out];

		int SrcIndex = -1;
		for (int ch = 0; ch < SrcChannels; ch++) {
			if (::av_channel_layout_channel_from_index(&pFrame->ch_layout, ch) == Channel) {
				SrcIndex = ch;
				break;
			}
		}

		if (SrcIndex < 0) {
			// 放送によってはこのchが元々存在しないことがある(無音とする)
			for (int i = 0; i < Samples; i++)
				pDst[static_cast<size_t>(i) * LatmSpatial::RAW_CHANNEL_COUNT + Out] = 0;
			continue;
		}

		const float *pSrc = reinterpret_cast<const float *>(pFrame->extended_data[SrcIndex]);
		for (int i = 0; i < Samples; i++) {
			float v = pSrc[i] * 32767.0f;
			if (v > 32767.0f)
				v = 32767.0f;
			else if (v < -32768.0f)
				v = -32768.0f;
			pDst[static_cast<size_t>(i) * LatmSpatial::RAW_CHANNEL_COUNT + Out] = static_cast<int16_t>(v);
		}
	}

	return LatmSpatial::RAW_CHANNEL_COUNT;
}


bool AACLATMDecoder_Libav::Decode(const uint8_t *pData, size_t *pDataSize, ReturnArg<DecodeFrameInfo> Info)
{
	if (!IsOpened() || (pData == nullptr) || (pDataSize == nullptr))
		return false;

	Info->SampleCount = 0;

	const uint8_t *pInData = pData;
	int InputSize = static_cast<int>(*pDataSize);

	uint8_t *pOutData = nullptr;
	int OutLen = 0;
	const int Consumed = ::av_parser_parse2(
		m_pParser, m_pCodecContext, &pOutData, &OutLen,
		pInData, InputSize,
		AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);

	*pDataSize = (Consumed > 0) ? static_cast<size_t>(Consumed) : *pDataSize;

	if (OutLen <= 0) {
		// まだ1フレーム分のデータが揃っていない
		if (Consumed <= 0)
			*pDataSize = InputSize; // 無限ループ防止のため、投入分は消費済みとする
		return true;
	}

	m_pPacket->data = pOutData;
	m_pPacket->size = OutLen;

	int Ret = ::avcodec_send_packet(m_pCodecContext, m_pPacket);
	if (Ret < 0) {
		LIBISDB_TRACE(LIBISDB_STR("AACLATMDecoder_Libav::Decode() avcodec_send_packet() error {}\n"), Ret);
		m_DecodeError = true;
		return true;
	}

	Ret = ::avcodec_receive_frame(m_pCodecContext, m_pFrame);
	if (Ret < 0) {
		// まだデコード結果が無い(内部バッファリング中)。エラーではない
		return true;
	}

	const int UsedChannels = ConvertFrame(m_pFrame, reinterpret_cast<int16_t *>(m_PCMBuffer.GetBuffer()));

	if (UsedChannels == 0) {
		// 「ダウンミックスしない」設定で8ch超の音声だった場合。
		// このフレームは出力しない(無音)
		::av_frame_unref(m_pFrame);
		return true;
	}

	if (m_pFrame->ch_layout.nb_channels != m_LastChannelCount) {
		LIBISDB_TRACE(
			LIBISDB_STR("AACLATMDecoder_Libav::Decode() Channel count changed {} -> {} (output {}ch)\n"),
			m_LastChannelCount, m_pFrame->ch_layout.nb_channels, UsedChannels);
		m_LastChannelCount = m_pFrame->ch_layout.nb_channels;
	}

	m_AudioInfo.Frequency = m_pFrame->sample_rate;
	m_AudioInfo.ChannelCount = UsedChannels;
	m_AudioInfo.OriginalChannelCount = m_pFrame->ch_layout.nb_channels;
	m_AudioInfo.DualMono = false;

	Info->pData = m_PCMBuffer.GetBuffer();
	Info->SampleCount = m_pFrame->nb_samples;
	Info->Info = m_AudioInfo;
	Info->Discontinuity = m_DecodeError;

	m_DecodeError = false;

	::av_frame_unref(m_pFrame);

	return true;
}


}	// namespace LibISDB::DirectShow


#endif	// LIBISDB_HAS_LIBAV_AAC
