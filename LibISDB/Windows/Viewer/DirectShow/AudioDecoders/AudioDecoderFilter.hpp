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
 @file   AudioDecoderFilter.hpp
 @brief  音声デコーダフィルタ
 @author DBCTRADO
*/


#ifndef LIBISDB_AUDIO_DECODER_FILTER_H
#define LIBISDB_AUDIO_DECODER_FILTER_H


#include "AudioDecoder.hpp"
#include "../DirectShowBase.hpp"
#include "../../../../Base/DataBuffer.hpp"
#include "../../../../Base/SIMD.hpp"
#include "../../../../Base/EventListener.hpp"
#include <atomic>
#include <memory>
#include <vector>


namespace LibISDB::DirectShow
{

	/** 音声デコーダフィルタクラス */
	class __declspec(uuid("2AD583EC-1D57-4d0d-8991-487F2A0A0E8B")) AudioDecoderFilter
		: public CTransformFilter
	{
	public:
		static IBaseFilter * WINAPI CreateInstance(LPUNKNOWN pUnk, HRESULT *phr);

		DECLARE_IUNKNOWN

	// CTransformFilter
		HRESULT CheckInputType(const CMediaType *mtIn) override;
		HRESULT CheckTransform(const CMediaType *mtIn, const CMediaType *mtOut) override;
		HRESULT DecideBufferSize(IMemAllocator *pAllocator, ALLOCATOR_PROPERTIES *pprop) override;
		HRESULT GetMediaType(int iPosition, CMediaType *pMediaType) override;
		HRESULT StartStreaming() override;
		HRESULT StopStreaming() override;
		HRESULT BeginFlush() override;
		HRESULT NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) override;
		HRESULT Receive(IMediaSample *pSample) override;

	// AudioDecoderFilter
		static constexpr BYTE ChannelCount_DualMono = 0x00;
		static constexpr BYTE ChannelCount_Invalid  = 0xFF;

		enum class DecoderType {
			Invalid,
			AAC,
			MPEGAudio,
			AC3,
#ifdef LIBISDB_HAS_FDK_AAC
			FDK_AAC,
			AAC_LATM,
#endif
#ifdef LIBISDB_HAS_LIBAV_AAC
			AAC_LATM_LIBAV,
#endif
		};

		enum class DualMonoMode {
			Invalid,
			Main,
			Sub,
			Both
		};

		enum class StereoMode {
			Stereo,
			Left,
			Right
		};

		enum class SPDIFMode {
			Disabled,
			Passthrough,
			Auto
		};

		struct SPDIFOptions {
			static constexpr UINT Channel_Mono     = 0x01U;
			static constexpr UINT Channel_Stereo   = 0x02U;
			static constexpr UINT Channel_DualMono = 0x04U;
			static constexpr UINT Channel_Surround = 0x08U;

			SPDIFMode Mode = SPDIFMode::Disabled;
			UINT PassthroughChannels = 0;

			SPDIFOptions() = default;
			SPDIFOptions(SPDIFMode mode, UINT channels) noexcept
				: Mode(mode), PassthroughChannels(channels) {}
			bool operator == (const SPDIFOptions &rhs) const noexcept = default;
		};

		struct SurroundMixingMatrix {
			double Matrix[6][6];
		};

		struct DownMixMatrix {
			double Matrix[2][6];
		};

		class EventListener : public LibISDB::EventListener
		{
		public:
			virtual void OnSPDIFPassthroughError(HRESULT hr) {}

			// 出力フォーマットが変わり、今接続されているレンダラへの
			// 再接続(ReconnectOutput())を試みたが拒否された時に呼ばれる。
			// (例: 22.2ch用のSpatialAudioRendererが繋がっている状態で
			//  番組が22.2chでなくなった場合、あるいはその逆)
			// 呼び出し側(ViewerFilter)は、これを受けてレンダラの
			// 繋ぎ直しを行うことを想定している
			virtual void OnAudioFormatChangeRejected() {}

			// 設定変更(SetLatmDownmixChannels())により、今接続されている
			// レンダラが不適切になる可能性があるため、レンダラの再確認
			// (必要なら繋ぎ直し)を依頼する時に呼ばれる。
			// OnAudioFormatChangeRejected()と異なり、呼び出しは設定を変更した
			// スレッド(通常はUIスレッド)から行われ、ストリーミングスレッド
			// からではない
			virtual void OnAudioRendererRecheckRequested() {}
		};

		class SampleCallback
		{
		public:
			virtual ~SampleCallback() = default;
			virtual void OnSamples(short *pData, size_t Length, int Channels) = 0;
		};

		using AudioInfo = AudioDecoder::AudioInfo;

		bool SetDecoderType(DecoderType Type);
		DecoderType GetDecoderType() const noexcept { return m_DecoderType; }
		uint8_t GetCurrentChannelCount() const;
		uint8_t GetOutputChannelCount() const;
		bool GetAudioInfo(ReturnArg<AudioInfo> Info) const;
		bool SetDualMonoMode(DualMonoMode Mode);
		DualMonoMode GetDualMonoMode() const noexcept { return m_DualMonoMode; }
		bool SetStereoMode(StereoMode Mode);
		StereoMode GetStereoMode() const noexcept { return m_StereoMode; }
		bool SetDownMixSurround(bool DownMix);
		bool GetDownMixSurround() const noexcept { return m_DownMixSurround; }

		// 22.2ch等、8chを超えるAAC-LATM音声のダウンミックス先チャンネル数を
		// 指定する。8=7.1ch(既定), 6=5.1ch, 0=ダウンミックスしない
		bool SetLatmDownmixChannels(int Channels);
		bool SetLatmDownmixPresetName(const LibISDB::CharType *pName);
		int GetLatmDownmixChannels() const noexcept { return m_LatmDownmixChannels; }
		bool SetSurroundMixingMatrix(const SurroundMixingMatrix *pMatrix);
		bool SetDownMixMatrix(const DownMixMatrix *pMatrix);
		bool SetGainControl(bool GainControl, float Gain = 1.0f, float SurroundGain = 1.0f);
		bool GetGainControl(float *pGain = nullptr, float *pSurroundGain = nullptr) const;
		bool SetJitterCorrection(bool Enable);
		bool GetJitterCorrection() const noexcept { return m_JitterCorrection; }
		bool SetDelay(LONGLONG Delay);
		LONGLONG GetDelay() const noexcept { return m_Delay; }
		bool SetSPDIFOptions(const SPDIFOptions &Options);
		bool GetSPDIFOptions(SPDIFOptions *pOptions) const;
		bool IsSPDIFPassthrough() const noexcept { return m_Passthrough; }

		bool AddEventListener(EventListener *pEventListener);
		bool RemoveEventListener(EventListener *pEventListener);
		bool SetSampleCallback(SampleCallback *pCallback);

		static bool GetDecoderVersion(DecoderType Type, std::string *pVersion);

	private:
		AudioDecoderFilter(LPUNKNOWN pUnk, HRESULT *phr);
		~AudioDecoderFilter();

	// CTransformFilter
		HRESULT Transform(IMediaSample *pIn, IMediaSample *pOut) override;

	// AudioDecoderFilter
		struct FrameSampleInfo {
			DataBuffer *pData;
			size_t SampleCount;
			bool MediaTypeChanged;
			CMediaType MediaType;
			long MediaBufferSize;
		};

		HRESULT OnFrame(
			const uint8_t *pData, size_t Samples,
			const AudioDecoder::AudioInfo &Info,
			FrameSampleInfo *pSampleInfo);
		HRESULT ProcessPCM(
			const uint8_t *pData, size_t Samples,
			const AudioDecoder::AudioInfo &Info,
			FrameSampleInfo *pSampleInfo);
		HRESULT ProcessSPDIF(
			const AudioDecoder::AudioInfo &Info,
			FrameSampleInfo *pSampleInfo);
		HRESULT ReconnectOutput(long BufferSize, const CMediaType &mt);
		void ResetSync();
		size_t MonoToStereo(int16_t *pDst, const int16_t *pSrc, size_t Samples);
		size_t DownMixStereo(int16_t *pDst, const int16_t *pSrc, size_t Samples);
		size_t DownMixSurround(int16_t *pDst, const int16_t *pSrc, size_t Samples);
		size_t MapSurroundChannels(int16_t *pDst, const int16_t *pSrc, size_t Samples);
		size_t DownMixSurround8(int16_t *pDst, const int16_t *pSrc, size_t Samples);
		size_t MapSurround8Channels(int16_t *pDst, const int16_t *pSrc, size_t Samples);
		void GainControl(int16_t *pBuffer, size_t Samples, float Gain);
		void SelectDualMonoStereoMode();
		AudioDecoder * CreateDecoder(DecoderType Type);

		DecoderType m_DecoderType;
		std::unique_ptr<AudioDecoder> m_Decoder;
		mutable CCritSec m_cPropLock;
		CMediaType m_MediaType;
		SSEDataBuffer m_OutData;
		uint8_t m_OutChannelNum;
		uint8_t m_CurChannelNum;
		std::vector<int16_t> m_ExpandWork; // 24ch拡張時の中間バッファ(ProcessPCM内でのみ使用)
		bool m_DualMono;

		DualMonoMode m_DualMonoMode;
		StereoMode m_StereoMode;
		bool m_DownMixSurround;
		int m_LatmDownmixChannels = 8;
		bool m_EnableCustomMixingMatrix;
		SurroundMixingMatrix m_MixingMatrix;
		bool m_EnableCustomDownMixMatrix;
		DownMixMatrix m_DownMixMatrix;

		bool m_GainControl;
		float m_Gain;
		float m_SurroundGain;

		bool m_JitterCorrection;
		LONGLONG m_Delay;
		LONGLONG m_DelayAdjustment;
		REFERENCE_TIME m_StartTime;
		LONGLONG m_SampleCount;
		bool m_Discontinuity;
		// レンダラの繋ぎ直し中は、新しい形式の出力を送らずに捨てる
		// (古いレンダラへ、受け付けられない形式のデータや形式変更を
		//  送ってしまうのを避けるため)
		std::atomic<bool> m_HoldOutput{false};
		bool m_InputDiscontinuity;

		SPDIFOptions m_SPDIFOptions;
		bool m_Passthrough;
		bool m_PassthroughError;

		EventListenerList<EventListener> m_EventListenerList;

		SampleCallback *m_pSampleCallback;
	};

} // namespace LibISDB::DirectShow


#endif // ifndef LIBISDB_AUDIO_DECODER_FILTER_H
