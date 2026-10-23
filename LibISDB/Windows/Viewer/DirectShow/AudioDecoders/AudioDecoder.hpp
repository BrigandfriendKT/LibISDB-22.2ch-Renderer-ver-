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
 @file   AudioDecoder.hpp
 @brief  音声デコーダ
 @author DBCTRADO
*/


#ifndef LIBISDB_AUDIO_DECODER_H
#define LIBISDB_AUDIO_DECODER_H


namespace LibISDB::DirectShow
{

	/** 音声デコーダ基底クラス */
	class AudioDecoder
	{
	public:
		enum {
			CHANNEL_2_L,
			CHANNEL_2_R
		};
		enum {
			CHANNEL_6_FL,
			CHANNEL_6_FR,
			CHANNEL_6_FC,
			CHANNEL_6_LFE,
			CHANNEL_6_BL,
			CHANNEL_6_BR
		};
		enum {
			CHANNEL_8_FL,
			CHANNEL_8_FR,
			CHANNEL_8_FC,
			CHANNEL_8_LFE,
			CHANNEL_8_BL,
			CHANNEL_8_BR,
			CHANNEL_8_SL,
			CHANNEL_8_SR
		};

		struct AudioInfo {
			long Frequency = 0;
			int ChannelCount = 0;
			int OriginalChannelCount = 0;
			bool DualMono = false;
		};

		struct DecodeFrameInfo {
			const uint8_t *pData = nullptr;
			size_t SampleCount = 0;
			AudioInfo Info;
			bool Discontinuity = false;
		};

		struct DownmixInfo {
			double Center = 0.0;
			double Front  = 0.0;
			double Rear   = 0.0;
			double LFE    = 0.0;
		};

		struct SPDIFFrameInfo {
			uint16_t Pc;
			int FrameSize;
			int SamplesPerFrame;
		};

		virtual ~AudioDecoder() = default;

		virtual bool Open() = 0;
		virtual void Close() = 0;
		virtual bool IsOpened() const = 0;
		virtual bool Reset() = 0;
		virtual bool Decode(const uint8_t *pData, size_t *pDataSize, ReturnArg<DecodeFrameInfo> Info) = 0;

		virtual bool IsSPDIFSupported() const { return false; }
		virtual bool GetSPDIFFrameInfo(ReturnArg<SPDIFFrameInfo> Info) const { return false; }
		virtual int GetSPDIFBurstPayload(uint8_t *pBuffer, size_t BufferSize) const { return false; }

		virtual bool GetChannelMap(int Channels, int *pMap) const { return false; }
		virtual bool GetDownmixInfo(ReturnArg<DownmixInfo> Info) const { return false; }

		// 22.2ch等、8chを超える音声のダウンミックス先チャンネル数を指定する
		// (対応しているデコーダのみ意味を持つ。それ以外は無視される)
		virtual void SetDownmixChannels(int Channels) {}

		// LatmDownmixPresets.iniのプリセットを明示的な名前で指定する。
		// nullptrまたは空文字列の場合、SetDownmixChannels()の値から決まる
		// 既定名が使われる(対応しているデコーダのみ意味を持つ)
		virtual void SetDownmixPresetName(const CharType *pName) {}

		bool GetAudioInfo(ReturnArg<AudioInfo> Info) const;

	protected:
		void ClearAudioInfo();

		AudioInfo m_AudioInfo;
	};

} // namespace LibISDB::DirectShow


#endif // ifndef LIBISDB_AUDIO_DECODER_H
