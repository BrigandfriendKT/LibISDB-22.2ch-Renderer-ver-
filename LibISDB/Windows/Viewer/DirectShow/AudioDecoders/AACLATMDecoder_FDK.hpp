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
 @file   AACLATMDecoder_FDK.hpp
 @brief  AAC-LATM(LOAS) デコーダ (FDK AAC)
 @author DBCTRADO

 MPEG-TS stream_type 0x11 (ISO/IEC 14496-3 Audio with the LATM transport
 syntax) 用のデコーダ。8K放送等で使用される。

 LOAS(Low Overhead Audio Stream)は自己同期可能なフレーム構造を持ち、
 AudioMuxElement / StreamMuxConfig / PayloadLengthInfo の解析は
 FDK-AACライブラリが TT_MP4_LOAS トランスポート型で内部的に行うため、
 ADTS用の AACDecoder(ADTSParser使用)とは異なり、PESペイロードの生バイト列を
 そのまま aacDecoder_Fill() に渡すだけでよい。
 そのため AudioDecoder を直接継承し、ADTSParser には依存しない。
*/


#ifndef LIBISDB_AAC_LATM_DECODER_FDK_H
#define LIBISDB_AAC_LATM_DECODER_FDK_H


#ifdef LIBISDB_HAS_FDK_AAC


#include "AudioDecoder.hpp"
#include "../../../../Base/DataBuffer.hpp"


typedef struct AAC_DECODER_INSTANCE *HANDLE_AACDECODER;


namespace LibISDB::DirectShow
{

	/** AAC-LATM(LOAS) デコーダクラス (FDK AAC) */
	class AACLATMDecoder_FDK
		: public AudioDecoder
	{
	public:
		AACLATMDecoder_FDK() noexcept;
		~AACLATMDecoder_FDK();

	// AudioDecoder
		bool Open() override;
		void Close() override;
		bool IsOpened() const override;
		bool Reset() override;
		bool Decode(const uint8_t *pData, size_t *pDataSize, ReturnArg<DecodeFrameInfo> Info) override;

		bool GetChannelMap(int Channels, int *pMap) const override;
		bool GetDownmixInfo(ReturnArg<DownmixInfo> Info) const override;

	private:
		bool OpenDecoder();
		void CloseDecoder();
		bool ResetDecoder();

		// 22.2ch(24ch)等、8chを超えるデコード結果を7.1ch(8ch)へミックスする。
		// FDK-AACのAAC_PCM_MAX_OUTPUT_CHANNELSは8ch超の入力には効かないため、
		// 自前でチャンネル種別(水平/垂直位置)に基づいたミックスを行う。
		void DownmixTo7_1(
			const int16_t *pSrc, int SrcChannels,
			const int *pChannelType, const uint8_t *pChannelIndices,
			int16_t *pDst, int FrameSize);

		HANDLE_AACDECODER m_hDecoder;
		int m_LastChannelCount;
		bool m_DecodeError;
		DataBuffer m_PCMBuffer;
		DataBuffer m_DownmixBuffer;
	};

} // namespace LibISDB::DirectShow


#endif // LIBISDB_HAS_FDK_AAC


#endif // ifndef LIBISDB_AAC_LATM_DECODER_FDK_H
