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
 @file   AACLATMDecoder_Libav.hpp
 @brief  AAC-LATM(LOAS) デコーダ (libavcodec)
 @author DBCTRADO

 FDK-AACは内部構造上8chまでしかデコードできないため、22.2ch(24ch)等の
 8chを超えるAAC-LATM音声はlibavcodec(FFmpeg由来)のAACデコーダで扱う。

 チャンネル数が8を超える場合は、LatmDownmixMatrix.hppのチャンネル
 マトリクスエンジンを使って7.1ch(8ch)/5.1ch(6ch)へダウンミックスする。
 ダウンミックス内容(どの入力chをどのゲインで混ぜるか)はLatmDownmixPresets.ini
 で自由に定義でき、iniが無い場合は組み込みのデフォルト定義が使われる。
 8ch以下の場合はダウンミックスせず、標準的な並び順
 (FL,FR,FC,LFE,BL,BR,SL,SR)へ並び替えるだけで出力する(この部分は
 マトリクスエンジンの対象外)。
*/


#ifndef LIBISDB_AAC_LATM_DECODER_LIBAV_H
#define LIBISDB_AAC_LATM_DECODER_LIBAV_H


#ifdef LIBISDB_HAS_LIBAV_AAC


#include "AudioDecoder.hpp"
#include "../../../../Base/DataBuffer.hpp"
#include <memory>


struct AVCodecContext;
struct AVCodecParserContext;
struct AVPacket;
struct AVFrame;

// LatmDownmixMatrix.hpp / LatmSpatialChannelMap.hpp (ffmpegのヘッダに
// 依存する)は、あえてこのヘッダではincludeしない。AudioDecoderFilter.cpp
// 等、ffmpeg用のインクルードパスが設定されていないファイルからも
// このヘッダはincludeされるため。実体は.cpp側でのみincludeする
namespace LibISDB::DirectShow::LatmDownmix
{
	struct Matrix;
}


namespace LibISDB::DirectShow
{

	/** AAC-LATM(LOAS) デコーダクラス (libavcodec) */
	class AACLATMDecoder_Libav
		: public AudioDecoder
	{
	public:
		AACLATMDecoder_Libav() noexcept;
		~AACLATMDecoder_Libav();

	// AudioDecoder
		bool Open() override;
		void Close() override;
		bool IsOpened() const override;
		bool Reset() override;
		bool Decode(const uint8_t *pData, size_t *pDataSize, ReturnArg<DecodeFrameInfo> Info) override;

		bool GetChannelMap(int Channels, int *pMap) const override;
		bool GetDownmixInfo(ReturnArg<DownmixInfo> Info) const override;

		// 8chを超える音声(22.2ch等)をダウンミックスする先のチャンネル数を
		// 指定する。8=7.1ch, 6=5.1ch, 2=2ch(ステレオ),
		// 0=ダウンミックスしない(生の24chで出力)。デフォルトは8(7.1ch)
		void SetDownmixChannels(int Channels) override;

		// LatmDownmixPresets.iniのプリセットを明示的な名前で指定する。
		// nullptrまたは空文字列の場合、SetDownmixChannels()の値から決まる
		// 既定名("7.1ch"/"5.1ch"/"2ch")が使われる
		void SetDownmixPresetName(const CharType *pName) override;

		static bool GetVersion(std::string *pVersion);

	private:
		bool OpenDecoder();
		void CloseDecoder();
		bool ResetDecoder();

		// デコード結果(frame)を、標準順序のPCMへ変換する。
		// 8chを超える場合はm_Matrixへダウンミックスする
		int ConvertFrame(const AVFrame *pFrame, int16_t *pDst);

		// 「ダウンミックスしない」設定時、22.2ch(24ch)を
		// LatmSpatial::RawChannelOrderの固定順序へ並び替えてそのまま出力する
		// (該当chが元々無ければ無音)。戻り値は常に24
		int ConvertFrameRaw24ch(const AVFrame *pFrame, int16_t *pDst);

		// m_DownmixChannelsに対応するプリセット("7.1ch"/"5.1ch"/"2ch")、
		// または m_PresetNameOverride が空でなければその名前を、
		// iniファイル(あれば)または組み込み定義から読み込み、m_Matrixへ格納する
		void ResolveDownmixMatrix();

		AVCodecContext *m_pCodecContext;
		AVCodecParserContext *m_pParser;
		AVPacket *m_pPacket;
		AVFrame *m_pFrame;

		int m_LastChannelCount;
		bool m_DecodeError;
		int m_DownmixChannels; // 8 / 6 / 2 / 0(無効)
		LibISDB::String m_PresetNameOverride; // 空ならm_DownmixChannelsから決まる既定名を使う
		std::unique_ptr<LatmDownmix::Matrix> m_Matrix; // m_DownmixChannelsが8/6/2の時に使用するマトリクス
		bool m_MatrixValid;          // m_Matrixが有効(解決済み)かどうか
		DataBuffer m_PCMBuffer;
	};

}	// namespace LibISDB::DirectShow


#endif	// LIBISDB_HAS_LIBAV_AAC


#endif	// ifndef LIBISDB_AAC_LATM_DECODER_LIBAV_H
