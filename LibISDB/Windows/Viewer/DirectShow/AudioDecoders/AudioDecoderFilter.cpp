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
 @file   AudioDecoderFilter.cpp
 @brief  音声デコーダフィルタ
 @author DBCTRADO
*/


#include "../../../../LibISDBPrivate.hpp"
#include "../../../../LibISDBWindows.hpp"
#include "AudioDecoderFilter.hpp"
#include "AACDecoder_FAAD2.hpp"
#ifdef LIBISDB_HAS_FDK_AAC
#include "AACDecoder_FDK.hpp"
#include "AACLATMDecoder_FDK.hpp"
#endif
#ifdef LIBISDB_HAS_LIBAV_AAC
#include "AACLATMDecoder_Libav.hpp"
#endif
#include "MPEGAudioDecoder.hpp"
#include "AC3Decoder.hpp"
#include <mmreg.h>
#include <cstring>
#include <cwchar>
#include "../../../../Base/DebugDef.hpp"


namespace LibISDB::DirectShow
{

namespace
{


// デフォルト周波数(48kHz)
constexpr int FREQUENCY = 48000;

// フレーム当たりのサンプル数(最大)
// AAC        : 4096
// MPEG Audio : 1152
// AC-3       : 1536 == 256 * 6
constexpr int SAMPLES_PER_FRAME = 4096;

// REFERENCE_TIMEの一秒
constexpr REFERENCE_TIME REFERENCE_TIME_SECOND = 10000000LL;

// サンプルのバッファサイズ
// (フレーム当たりのサンプル数 * 16ビット * 5.1ch)
constexpr long SAMPLE_BUFFER_SIZE = SAMPLES_PER_FRAME * 2 * 24; // 22.2ch(24ch)まで対応

// MediaSample用に確保するバッファ数
constexpr long NUM_SAMPLE_BUFFERS = 4;

// ジッタの許容上限
constexpr REFERENCE_TIME MAX_JITTER = REFERENCE_TIME_SECOND / 5LL;


template<typename T> constexpr int16_t ClampSample16(T v)
{
	return v > std::numeric_limits<int16_t>::max() ? std::numeric_limits<int16_t>::max() :
	       v < std::numeric_limits<int16_t>::min() ? std::numeric_limits<int16_t>::min() :
	       static_cast<T>(v);
}


} // namespace




AudioDecoderFilter::AudioDecoderFilter(LPUNKNOWN pUnk, HRESULT *phr)
	: CTransformFilter(TEXT("Audio Decoder Filter"), pUnk, __uuidof(AudioDecoderFilter))
	, m_DecoderType(DecoderType::Invalid)
	, m_OutChannelNum(0)
	, m_CurChannelNum(0)
	, m_DualMono(false)

	, m_DualMonoMode(DualMonoMode::Main)
	, m_StereoMode(StereoMode::Stereo)
	, m_DownMixSurround(true)
	, m_EnableCustomMixingMatrix(false)
	, m_EnableCustomDownMixMatrix(false)

	, m_GainControl(false)
	, m_Gain(1.0f)
	, m_SurroundGain(1.0f)

	, m_JitterCorrection(false)
	, m_Delay(0)
	, m_DelayAdjustment(0)
	, m_StartTime(-1)
	, m_SampleCount(0)
	, m_Discontinuity(true)
	, m_InputDiscontinuity(true)

	, m_Passthrough(false)
	, m_PassthroughError(false)

	, m_pSampleCallback(nullptr)
{
	LIBISDB_TRACE(LIBISDB_STR("AudioDecoderFilter::AudioDecoderFilter {}\n"), static_cast<void *>(this));

	*phr = S_OK;

	// メディアタイプ設定
	m_MediaType.InitMediaType();
	m_MediaType.SetType(&MEDIATYPE_Audio);
	m_MediaType.SetSubtype(&MEDIASUBTYPE_PCM);
	m_MediaType.SetTemporalCompression(FALSE);
	m_MediaType.SetSampleSize(0);
	m_MediaType.SetFormatType(&FORMAT_WaveFormatEx);

	 // フォーマット構造体確保
#if 1
	// 2ch
	WAVEFORMATEX *pWaveInfo = reinterpret_cast<WAVEFORMATEX *>(m_MediaType.AllocFormatBuffer(sizeof(WAVEFORMATEX)));
	if (pWaveInfo == nullptr) {
		*phr = E_OUTOFMEMORY;
		return;
	}
	// WAVEFORMATEX構造体設定(48kHz 16bit ステレオ固定)
	pWaveInfo->wFormatTag = WAVE_FORMAT_PCM;
	pWaveInfo->nChannels = 2;
	pWaveInfo->nSamplesPerSec = FREQUENCY;
	pWaveInfo->wBitsPerSample = 16;
	pWaveInfo->nBlockAlign = pWaveInfo->wBitsPerSample * pWaveInfo->nChannels / 8;
	pWaveInfo->nAvgBytesPerSec = pWaveInfo->nSamplesPerSec * pWaveInfo->nBlockAlign;
	pWaveInfo->cbSize = 0;
#else
	// 5.1ch
	WAVEFORMATEXTENSIBLE *pWaveInfo = reinterpret_cast<WAVEFORMATEXTENSIBLE *>(m_MediaType.AllocFormatBuffer(sizeof(WAVEFORMATEXTENSIBLE)));
	if (pWaveInfo == nullptr) {
		*phr = E_OUTOFMEMORY;
		return;
	}
	// WAVEFORMATEXTENSIBLE構造体設定(48KHz 16bit 5.1ch固定)
	pWaveInfo->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	pWaveInfo->Format.nChannels = 6;
	pWaveInfo->Format.nSamplesPerSec = FREQUENCY;
	pWaveInfo->Format.wBitsPerSample = 16;
	pWaveInfo->Format.nBlockAlign = pWaveInfo->Format.wBitsPerSample * pWaveInfo->Format.nChannels / 8;
	pWaveInfo->Format.nAvgBytesPerSec = pWaveInfo->Format.nSamplesPerSec * pWaveInfo->Format.nBlockAlign;
	pWaveInfo->Format.cbSize  = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	pWaveInfo->dwChannelMask =
		SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
		SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
		SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
	pWaveInfo->Samples.wValidBitsPerSample = 16;
	pWaveInfo->SubFormat = MEDIASUBTYPE_PCM;
#endif

	if (m_OutData.AllocateBuffer(SAMPLE_BUFFER_SIZE) < SAMPLE_BUFFER_SIZE)
		*phr = E_OUTOFMEMORY;
}


AudioDecoderFilter::~AudioDecoderFilter()
{
	LIBISDB_TRACE(LIBISDB_STR("AudioDecoderFilter::~AudioDecoderFilter\n"));
}


IBaseFilter * WINAPI AudioDecoderFilter::CreateInstance(LPUNKNOWN pUnk, HRESULT *phr)
{
	// インスタンスを作成する
	AudioDecoderFilter *pNewFilter = new AudioDecoderFilter(pUnk, phr);
	if (FAILED(*phr))
		goto OnError;

	IBaseFilter *pFilter;
	*phr = pNewFilter->QueryInterface(IID_PPV_ARGS(&pFilter));
	if (FAILED(*phr))
		goto OnError;

	return pFilter;

OnError:
	delete pNewFilter;
	return nullptr;
}


HRESULT AudioDecoderFilter::CheckInputType(const CMediaType *mtIn)
{
	CheckPointer(mtIn, E_POINTER);

	// 何でもOK

	return S_OK;
}


HRESULT AudioDecoderFilter::CheckTransform(const CMediaType *mtIn, const CMediaType *mtOut)
{
	CheckPointer(mtIn, E_POINTER);
	CheckPointer(mtOut, E_POINTER);

	if (*mtOut->Type() == MEDIATYPE_Audio) {
		if (*mtOut->Subtype() == MEDIASUBTYPE_PCM) {

			// GUID_NULLではデバッグアサートが発生するのでダミーを設定して回避
			CMediaType MediaType;
			MediaType.InitMediaType();
			MediaType.SetType(&MEDIATYPE_Stream);
			MediaType.SetSubtype(&MEDIASUBTYPE_None);

			m_pInput->SetMediaType(&MediaType);

			return S_OK;
		}
	}

	return VFW_E_TYPE_NOT_ACCEPTED;
}


HRESULT AudioDecoderFilter::DecideBufferSize(IMemAllocator *pAllocator, ALLOCATOR_PROPERTIES *pprop)
{
	CheckPointer(pAllocator, E_POINTER);
	CheckPointer(pprop, E_POINTER);

	if (pprop->cBuffers < NUM_SAMPLE_BUFFERS)
		pprop->cBuffers = NUM_SAMPLE_BUFFERS;

	if (pprop->cbBuffer < SAMPLE_BUFFER_SIZE)
		pprop->cbBuffer = SAMPLE_BUFFER_SIZE;

	// アロケータプロパティを設定しなおす
	ALLOCATOR_PROPERTIES Actual;
	const HRESULT hr = pAllocator->SetProperties(pprop, &Actual);
	if (FAILED(hr))
		return hr;

	// 要求を受け入れられたか判定
	if ((Actual.cBuffers < pprop->cBuffers) || (Actual.cbBuffer < pprop->cbBuffer))
		return E_FAIL;

	return S_OK;
}


HRESULT AudioDecoderFilter::GetMediaType(int iPosition, CMediaType *pMediaType)
{
	CheckPointer(pMediaType, E_POINTER);
	CAutoLock AutoLock(m_pLock);

	if (iPosition < 0)
		return E_INVALIDARG;
	if (iPosition > 0)
		return VFW_S_NO_MORE_ITEMS;

	*pMediaType = m_MediaType;

	return S_OK;
}


HRESULT AudioDecoderFilter::StartStreaming()
{
	CAutoLock AutoLock(&m_cPropLock);

	if (m_Decoder) {
		m_Decoder->Open();
	}

	ResetSync();

	m_PassthroughError = false;

	return S_OK;
}


HRESULT AudioDecoderFilter::StopStreaming()
{
	CAutoLock AutoLock(&m_cPropLock);

	if (m_Decoder) {
		m_Decoder->Close();
	}

	return S_OK;
}


HRESULT AudioDecoderFilter::BeginFlush()
{
	const HRESULT hr = CTransformFilter::BeginFlush();

	CAutoLock AutoLock(&m_cPropLock);

	if (m_Decoder) {
		m_Decoder->Reset();
	}

	ResetSync();

	return hr;
}


HRESULT AudioDecoderFilter::NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate)
{
	const HRESULT hr = CTransformFilter::NewSegment(tStart, tStop, dRate);

	CAutoLock AutoLock(&m_cPropLock);

	ResetSync();

	return hr;
}


HRESULT AudioDecoderFilter::Transform(IMediaSample *pIn, IMediaSample *pOut)
{
	// 入力データポインタを取得する
	const size_t InSize = pIn->GetActualDataLength();
	BYTE *pInData = nullptr;
	HRESULT hr = pIn->GetPointer(&pInData);
	if (FAILED(hr))
		return hr;

	{
		CAutoLock Lock(&m_cPropLock);

		if (!m_Decoder) {
			m_Decoder.reset(CreateDecoder(m_DecoderType));
			if (!m_Decoder)
				return E_UNEXPECTED;
			m_Decoder->SetDownmixChannels(m_LatmDownmixChannels);
			m_Decoder->Open();
		}

		REFERENCE_TIME rtStart, rtEnd;
		hr = pIn->GetTime(&rtStart, &rtEnd);
		if (FAILED(hr))
			rtStart = -1;
		if (pIn->IsDiscontinuity() == S_OK) {
			m_Discontinuity = true;
			m_InputDiscontinuity = true;
		} else if ((hr == S_OK) || (hr == VFW_S_NO_STOP_TIME)) {
			if (!m_JitterCorrection) {
				m_StartTime = rtStart;
			} else if ((m_StartTime >= 0)
					&& (std::llabs(rtStart - m_StartTime) > MAX_JITTER)) {
				LIBISDB_TRACE(
					LIBISDB_STR("Resync audio stream time ({} -> {} [{}])\n"),
					m_StartTime, rtStart,
					static_cast<double>(rtStart - m_StartTime) / static_cast<double>(REFERENCE_TIME_SECOND));
				m_StartTime = rtStart;
			}
		}
		if ((m_StartTime < 0) || m_Discontinuity) {
			LIBISDB_TRACE(LIBISDB_STR("Initialize audio stream time ({})\n"), rtStart);
			m_StartTime = rtStart;
		}
	}

	size_t InDataPos = 0;
	FrameSampleInfo SampleInfo;
	SampleInfo.pData = &m_OutData;

	hr = S_OK;

	while (InDataPos < InSize) {
		AudioDecoder::DecodeFrameInfo FrameInfo;

		{
			CAutoLock Lock(&m_cPropLock);

			const size_t DataSize = InSize - InDataPos;
			size_t DecodeSize = DataSize;
			if (!m_Decoder->Decode(&pInData[InDataPos], &DecodeSize, &FrameInfo)) {
				if (DecodeSize < DataSize) {
					InDataPos += DecodeSize;
					continue;
				}
				break;
			}
			InDataPos += DecodeSize;

			if (FrameInfo.SampleCount == 0)
				continue;

			if (FrameInfo.Discontinuity)
				m_Discontinuity = true;

			// レンダラの繋ぎ直し中は出力を捨てる(SetLatmDownmixChannels()参照)
			if (m_HoldOutput.load()) {
				hr = S_OK;
				continue;
			}

			SampleInfo.MediaTypeChanged = false;

			hr = OnFrame(FrameInfo.pData, FrameInfo.SampleCount, FrameInfo.Info, &SampleInfo);
		}

		if (SUCCEEDED(hr)) {
			LIBISDB_TRACE(
				LIBISDB_STR("[DIAG] MediaTypeChanged={} SampleCount={} Channels={}\n"),
				SampleInfo.MediaTypeChanged, FrameInfo.SampleCount, FrameInfo.Info.ChannelCount);

			if (SampleInfo.MediaTypeChanged) {
				hr = ReconnectOutput(SampleInfo.MediaBufferSize, SampleInfo.MediaType);
				LIBISDB_TRACE(LIBISDB_STR("[DIAG] ReconnectOutput() returned hr={:08x}\n"), static_cast<unsigned int>(hr));
				if (FAILED(hr)) {
					// 今繋がっているレンダラが新しいフォーマットを
					// 受け付けなかった。レンダラごとの繋ぎ直しが
					// 必要な場合があるため、上位(ViewerFilter)へ通知する
					m_EventListenerList.CallEventListener(&EventListener::OnAudioFormatChangeRejected);
					break;
				}
				LIBISDB_TRACE(LIBISDB_STR("出力メディアタイプを更新します。\n"));
				hr = m_pOutput->SetMediaType(&SampleInfo.MediaType);
				if (FAILED(hr)) {
					LIBISDB_TRACE(LIBISDB_STR("出力メディアタイプを設定できません。({:08x})\n"), hr);
					break;
				}
				m_MediaType = SampleInfo.MediaType;
				m_Discontinuity = true;
				m_InputDiscontinuity = true;
			}

			IMediaSample *pOutSample = nullptr;
			hr = m_pOutput->GetDeliveryBuffer(&pOutSample, nullptr, nullptr, 0);
			if (FAILED(hr)) {
				LIBISDB_TRACE(LIBISDB_STR("出力メディアサンプルを取得できません。({:08x})\n"), hr);
				break;
			}

			if (SampleInfo.MediaTypeChanged)
				pOutSample->SetMediaType(&m_MediaType);

			// 出力ポインタ取得
			BYTE *pOutBuff = nullptr;
			hr = pOutSample->GetPointer(&pOutBuff);
			if (FAILED(hr)) {
				LIBISDB_TRACE(LIBISDB_STR("出力サンプルのバッファを取得できません。({:08x})\n"), hr);
				pOutSample->Release();
				break;
			}

			std::memcpy(pOutBuff, m_OutData.GetData(), m_OutData.GetSize());
			pOutSample->SetActualDataLength(static_cast<long>(m_OutData.GetSize()));

			if (m_StartTime >= 0) {
				REFERENCE_TIME rtDuration, rtStart, rtEnd;
				rtDuration = REFERENCE_TIME_SECOND * static_cast<REFERENCE_TIME>(SampleInfo.SampleCount) / FrameInfo.Info.Frequency;
				rtStart = m_StartTime;
				m_StartTime += rtDuration;
				// 音ずれ補正用時間シフト
				if (m_DelayAdjustment > 0) {
					// 最大2倍まで時間を遅らせる
					if (rtDuration >= m_DelayAdjustment) {
						rtDuration += m_DelayAdjustment;
						m_DelayAdjustment = 0;
					} else {
						m_DelayAdjustment -= rtDuration;
						rtDuration *= 2;
					}
				} else if (m_DelayAdjustment < 0) {
					// 最短1/2まで時間を早める
					if (rtDuration >= -m_DelayAdjustment * 2) {
						rtDuration += m_DelayAdjustment;
						m_DelayAdjustment = 0;
					} else {
						m_DelayAdjustment += rtDuration;
						rtDuration /= 2;
					}
				} else {
					rtStart += m_Delay;
				}
				rtEnd = rtStart + rtDuration;
				pOutSample->SetTime(&rtStart, &rtEnd);
			}
			pOutSample->SetMediaTime(nullptr, nullptr);
			pOutSample->SetPreroll(FALSE);
#if 0
			// Discontinuityを設定すると倍速再生がおかしくなる模様
			pOutSample->SetDiscontinuity(m_Discontinuity);
#else
			pOutSample->SetDiscontinuity(m_InputDiscontinuity);
#endif
			m_Discontinuity = false;
			m_InputDiscontinuity = false;
			pOutSample->SetSyncPoint(TRUE);

			hr = m_pOutput->Deliver(pOutSample);
			if (FAILED(hr)) {
				LIBISDB_TRACE(
					LIBISDB_STR("[DIAG] m_pOutput->Deliver() failed hr={:08x} (Channels={}, Freq={}, SampleSize={})\n"),
					static_cast<unsigned int>(hr), pOutSample->GetActualDataLength(),
					FrameInfo.Info.Frequency, m_OutData.GetSize());
#ifdef _DEBUG
				if (m_Passthrough && !m_PassthroughError) {
					m_PassthroughError = true;
					m_EventListenerList.CallEventListener(&EventListener::OnSPDIFPassthroughError, hr);
				}
#endif
			}
			pOutSample->Release();
			if (FAILED(hr))
				break;
		}
	}

	return hr;
}


HRESULT AudioDecoderFilter::Receive(IMediaSample *pSample)
{
	const AM_SAMPLE2_PROPERTIES *pProps = m_pInput->SampleProps();
	if (pProps->dwStreamId != AM_STREAM_MEDIA)
		return m_pOutput->Deliver(pSample);

	HRESULT hr;

	hr = Transform(pSample, nullptr);
	if (SUCCEEDED(hr)) {
		hr = S_OK;
		m_bSampleSkipped = FALSE;
	}

	return hr;
}


bool AudioDecoderFilter::SetDecoderType(DecoderType Type)
{
	CAutoLock AutoLock(&m_cPropLock);

	if (m_Decoder) {
		AudioDecoder *pDecoder = CreateDecoder(Type);
		if (!pDecoder)
			return false;
		m_Decoder.reset(pDecoder);
		m_Decoder->SetDownmixChannels(m_LatmDownmixChannels);
		m_Decoder->Open();
	}

	m_DecoderType = Type;

	return true;
}


uint8_t AudioDecoderFilter::GetCurrentChannelCount() const
{
	CAutoLock AutoLock(&m_cPropLock);

	if (m_CurChannelNum == 0)
		return ChannelCount_Invalid;
	if (m_DualMono)
		return ChannelCount_DualMono;
	return m_CurChannelNum;
}


uint8_t AudioDecoderFilter::GetOutputChannelCount() const
{
	CAutoLock AutoLock(&m_cPropLock);

	if (m_CurChannelNum == 0)
		return ChannelCount_Invalid;
	if (m_Passthrough)
		return m_CurChannelNum;
	if (m_OutChannelNum == 0)
		return ChannelCount_Invalid;
	return m_OutChannelNum;
}



bool AudioDecoderFilter::GetAudioInfo(ReturnArg<AudioInfo> Info) const
{
	CAutoLock AutoLock(&m_cPropLock);

	if (!m_Decoder)
		return false;

	return m_Decoder->GetAudioInfo(Info);
}


bool AudioDecoderFilter::SetDualMonoMode(DualMonoMode Mode)
{
	CAutoLock AutoLock(&m_cPropLock);

	switch (Mode) {
	case DualMonoMode::Invalid:
	case DualMonoMode::Main:
	case DualMonoMode::Sub:
	case DualMonoMode::Both:
		LIBISDB_TRACE(
			LIBISDB_STR("AudioDecoderFilter::SetDualMonoMode() : Mode {}\n"),
			static_cast<std::underlying_type_t<DualMonoMode>>(Mode));
		m_DualMonoMode = Mode;
		if (m_DualMono)
			SelectDualMonoStereoMode();
		return true;
	}

	return false;
}


bool AudioDecoderFilter::SetStereoMode(StereoMode Mode)
{
	CAutoLock AutoLock(&m_cPropLock);

	switch (Mode) {
	case StereoMode::Stereo:
	case StereoMode::Left:
	case StereoMode::Right:
		m_StereoMode = Mode;
		LIBISDB_TRACE(
			LIBISDB_STR("AudioDecoderFilter::SetStereoMode() : Stereo mode {}\n"),
			static_cast<std::underlying_type_t<StereoMode>>(Mode));
		return true;
	}

	return false;
}


bool AudioDecoderFilter::SetDownMixSurround(bool DownMix)
{
	CAutoLock AutoLock(&m_cPropLock);

	m_DownMixSurround = DownMix;
	return true;
}


bool AudioDecoderFilter::SetLatmDownmixChannels(int Channels)
{
	// 診断用: この設定がいつ(グラフ構築のどの段階で)呼ばれているかを
	// 確認するための出力。原因が分かったら削除して良い
	{
		wchar_t szBuff[128];
		swprintf_s(szBuff, L"[LatmSpatial] SetLatmDownmixChannels(%d) called\n", Channels);
		::OutputDebugStringW(szBuff);
	}

	if ((Channels != 8) && (Channels != 6) && (Channels != 2) && (Channels != 0))
		return false;

	bool NeedRendererCheck = (Channels == 0);

	{
		CAutoLock AutoLock(&m_cPropLock);

		// 今「22.2ch(24ch)用」の出力(空間オーディオ用レンダラ)が接続されている
		// 状態から、24ch以外へ変更される場合も、レンダラの再確認が必要。
		// (以前は、2chへ変更した時に出力形式の「仮の値」だけが2chへ上書きされ、
		//  ProcessPCM()が「形式は変わっていない」と判断してしまい、24chのまま
		//  接続されているレンダラへ2chのデータが送られ続けていた。
		//  24chとして読まれた2chデータは約12倍の速さで再生されるため、
		//  甲高くて聞き取れない音になっていた)
		if ((Channels != 0) && (m_pOutput != nullptr) && m_pOutput->IsConnected()) {
			const CMediaType &Connected = m_pOutput->CurrentMediaType();
			if ((*Connected.FormatType() == FORMAT_WaveFormatEx)
					&& (Connected.FormatLength() >= sizeof(WAVEFORMATEX))
					&& (reinterpret_cast<const WAVEFORMATEX *>(Connected.Format())->nChannels == 24))
				NeedRendererCheck = true;
		}

		// 繋ぎ直しが終わるまで、デコーダの出力は送らずに捨てる
		if (NeedRendererCheck)
			m_HoldOutput = true;

		m_LatmDownmixChannels = Channels;
		if (m_Decoder)
			m_Decoder->SetDownmixChannels(m_LatmDownmixChannels);

	// 出力ピンの「仮の」メディアタイプ(GetMediaType()が、実際のチャンネル数が
	// まだ判明していない最初の接続時に提案する内容)を、ここで更新しておく。
	// これをしないと、最初の接続では常に2ch(コンストラクタでの初期値)が
	// 提案されてしまい、「ダウンミックスしない」設定でも通常の音声レンダラが
	// 先に選ばれてしまう(22.2chと判明するのは最初の1フレームをデコードした
	// 後のため、SpatialAudioRendererが接続の候補にすら上がれなくなる)。
	// ※この変更を反映するには、チャンネルの再選局等でグラフを再構築する
	//   必要がある(接続済みのグラフへ即座に反映されるわけではない)
	if (Channels == 0) {
		// 22.2ch(24ch)。SpatialAudioRendererが受け付ける形式に合わせる
		m_MediaType.InitMediaType();
		m_MediaType.SetType(&MEDIATYPE_Audio);
		m_MediaType.SetSubtype(&MEDIASUBTYPE_PCM);
		m_MediaType.SetTemporalCompression(FALSE);
		m_MediaType.SetSampleSize(0);
		m_MediaType.SetFormatType(&FORMAT_WaveFormatEx);

		WAVEFORMATEXTENSIBLE *pWaveInfo = reinterpret_cast<WAVEFORMATEXTENSIBLE *>(
			m_MediaType.AllocFormatBuffer(sizeof(WAVEFORMATEXTENSIBLE)));
		if (pWaveInfo != nullptr) {
			pWaveInfo->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
			pWaveInfo->Format.nChannels = 24;
			pWaveInfo->Format.nSamplesPerSec = FREQUENCY;
			pWaveInfo->Format.wBitsPerSample = 16;
			pWaveInfo->Format.nBlockAlign =
				pWaveInfo->Format.wBitsPerSample * pWaveInfo->Format.nChannels / 8;
			pWaveInfo->Format.nAvgBytesPerSec =
				pWaveInfo->Format.nSamplesPerSec * pWaveInfo->Format.nBlockAlign;
			pWaveInfo->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
			// Windows標準のdwChannelMaskには22.2ch全chぶんの定義が無いため、
			// 0(チャンネルマスク未指定)を指定する
			pWaveInfo->dwChannelMask = 0;
			pWaveInfo->SubFormat = MEDIASUBTYPE_PCM;
			pWaveInfo->Samples.wValidBitsPerSample = pWaveInfo->Format.wBitsPerSample;
		}
	} else {
		// 6ch/8ch/2chダウンミックス。2chを仮のフォーマットとしておく
		// (SpatialAudioRendererはこの形式では接続候補にならず、
		//  通常のレンダラが選ばれた後、実際のチャンネル数判明時に
		//  ReconnectOutput()で6ch/8chへ再接続される。これは元々の
		//  7.1ch/5.1chダウンミックスと同じ、既に動作確認済みの経路。
		//  Channels==2(2chダウンミックス)の場合は、この仮フォーマットが
		//  そのまま最終フォーマットと一致するため、再接続は発生しない)
		m_MediaType.InitMediaType();
		m_MediaType.SetType(&MEDIATYPE_Audio);
		m_MediaType.SetSubtype(&MEDIASUBTYPE_PCM);
		m_MediaType.SetTemporalCompression(FALSE);
		m_MediaType.SetSampleSize(0);
		m_MediaType.SetFormatType(&FORMAT_WaveFormatEx);

		WAVEFORMATEX *pWaveInfo = reinterpret_cast<WAVEFORMATEX *>(
			m_MediaType.AllocFormatBuffer(sizeof(WAVEFORMATEX)));
		if (pWaveInfo != nullptr) {
			pWaveInfo->wFormatTag = WAVE_FORMAT_PCM;
			pWaveInfo->nChannels = 2;
			pWaveInfo->nSamplesPerSec = FREQUENCY;
			pWaveInfo->wBitsPerSample = 16;
			pWaveInfo->nBlockAlign = pWaveInfo->wBitsPerSample * pWaveInfo->nChannels / 8;
			pWaveInfo->nAvgBytesPerSec = pWaveInfo->nSamplesPerSec * pWaveInfo->nBlockAlign;
			pWaveInfo->cbSize = 0;
		}
	}
	}

	if (NeedRendererCheck) {
		// 「ダウンミックスしない」が選ばれた。今接続されているレンダラが
		// 22.2ch用のものでない可能性があるため(特に、グラフ構築時に
		// まだこの設定が適用されていなかった場合)、上位(ViewerFilter)へ
		// 繋ぎ直しを促す。ViewerFilter側で「今のレンダラが適切かどうか」を
		// 判断して、不要であれば繋ぎ直しは行わない
		m_EventListenerList.CallEventListener(&EventListener::OnAudioRendererRecheckRequested);
		m_HoldOutput = false;
	}

	return true;
}


bool AudioDecoderFilter::SetLatmDownmixPresetName(const LibISDB::CharType *pName)
{
	CAutoLock AutoLock(&m_cPropLock);

	if (m_Decoder)
		m_Decoder->SetDownmixPresetName(pName);

	return true;
}


bool AudioDecoderFilter::SetSurroundMixingMatrix(const SurroundMixingMatrix *pMatrix)
{
	CAutoLock AutoLock(&m_cPropLock);

	if (pMatrix) {
		m_EnableCustomMixingMatrix = true;
		m_MixingMatrix = *pMatrix;
	} else {
		m_EnableCustomMixingMatrix = false;
	}

	return true;
}


bool AudioDecoderFilter::SetDownMixMatrix(const DownMixMatrix *pMatrix)
{
	CAutoLock AutoLock(&m_cPropLock);

	if (pMatrix) {
		m_EnableCustomDownMixMatrix = true;
		m_DownMixMatrix = *pMatrix;
	} else {
		m_EnableCustomDownMixMatrix = false;
	}

	return true;
}


bool AudioDecoderFilter::SetGainControl(bool GainControl, float Gain, float SurroundGain)
{
	CAutoLock AutoLock(&m_cPropLock);

	m_GainControl = GainControl;
	m_Gain = Gain;
	m_SurroundGain = SurroundGain;
	return true;
}


bool AudioDecoderFilter::GetGainControl(float *pGain, float *pSurroundGain) const
{
	CAutoLock AutoLock(&m_cPropLock);

	if (pGain)
		*pGain = m_Gain;
	if (pSurroundGain)
		*pSurroundGain = m_SurroundGain;
	return m_GainControl;
}


bool AudioDecoderFilter::SetSPDIFOptions(const SPDIFOptions &Options)
{
	CAutoLock AutoLock(&m_cPropLock);

	m_SPDIFOptions = Options;

	return true;
}


bool AudioDecoderFilter::GetSPDIFOptions(SPDIFOptions *pOptions) const
{
	if (!pOptions)
		return false;

	CAutoLock AutoLock(&m_cPropLock);

	*pOptions = m_SPDIFOptions;

	return true;
}


bool AudioDecoderFilter::SetJitterCorrection(bool Enable)
{
	CAutoLock AutoLock(&m_cPropLock);

	if (m_JitterCorrection != Enable) {
		m_JitterCorrection = Enable;

		m_StartTime = -1;
		m_SampleCount = 0;
	}

	return true;
}


bool AudioDecoderFilter::SetDelay(LONGLONG Delay)
{
	CAutoLock AutoLock(&m_cPropLock);

	LIBISDB_TRACE(LIBISDB_STR("AudioDecoderFilter::SetDelay() : {}\n"), Delay);

	m_DelayAdjustment += Delay - m_Delay;
	m_Delay = Delay;

	return true;
}


bool AudioDecoderFilter::SetSampleCallback(SampleCallback *pCallback)
{
	CAutoLock AutoLock(&m_cPropLock);

	m_pSampleCallback = pCallback;

	return true;
}


bool AudioDecoderFilter::AddEventListener(EventListener *pEventListener)
{
	return m_EventListenerList.AddEventListener(pEventListener);
}


bool AudioDecoderFilter::RemoveEventListener(EventListener *pEventListener)
{
	return m_EventListenerList.RemoveEventListener(pEventListener);
}


HRESULT AudioDecoderFilter::OnFrame(
	const uint8_t *pData, size_t Samples,
	const AudioDecoder::AudioInfo &Info,
	FrameSampleInfo *pSampleInfo)
{
	if (Info.ChannelCount != 1 && Info.ChannelCount != 2
			&& Info.ChannelCount != 6 && Info.ChannelCount != 8
			&& Info.ChannelCount != 24)
		return E_FAIL;

	const bool DualMono = (Info.ChannelCount == 2 && Info.DualMono);

	bool Passthrough = false;
	if (m_Decoder->IsSPDIFSupported()) {
		if (m_SPDIFOptions.Mode == SPDIFMode::Passthrough) {
			Passthrough = true;
		} else if (m_SPDIFOptions.Mode == SPDIFMode::Auto) {
			UINT ChannelFlag = 0;
			if (DualMono) {
				ChannelFlag = SPDIFOptions::Channel_DualMono;
			} else {
				// 「ダウンミックスしない」時は、元が8ch以下の音声も24chに
				// 拡張されて渡されるため、その場合は元のチャンネル数で判定する
				int FlagChannels = Info.ChannelCount;
				if ((Info.ChannelCount == 24) && (Info.OriginalChannelCount > 0)
						&& (Info.OriginalChannelCount <= 8))
					FlagChannels = Info.OriginalChannelCount;
				switch (FlagChannels) {
				case 1: ChannelFlag = SPDIFOptions::Channel_Mono;     break;
				case 2: ChannelFlag = SPDIFOptions::Channel_Stereo;   break;
				case 6: ChannelFlag = SPDIFOptions::Channel_Surround; break;
				}
			}
			if ((m_SPDIFOptions.PassthroughChannels & ChannelFlag) != 0)
				Passthrough = true;
		}
	}

	if (m_Passthrough != Passthrough)
		m_PassthroughError = false;
	m_Passthrough = Passthrough;

	if (DualMono != m_DualMono) {
		m_DualMono = DualMono;
		if (DualMono) {
			SelectDualMonoStereoMode();
		} else {
			m_StereoMode = StereoMode::Stereo;
		}
	}

	m_CurChannelNum = Info.OriginalChannelCount;

	HRESULT hr;

	if (m_Passthrough) {
		hr = ProcessSPDIF(Info, pSampleInfo);
	} else {
		hr = ProcessPCM(pData, Samples, Info, pSampleInfo);
	}

	return hr;
}


HRESULT AudioDecoderFilter::ProcessPCM(
	const uint8_t *pData, size_t Samples,
	const AudioDecoder::AudioInfo &Info,
	FrameSampleInfo *pSampleInfo)
{
	// 22.2ch(24ch、ダウンミックスしない設定でのAAC-LATM生データ)は、
	// m_DownMixSurround(2chへの折り込み)設定に関わらず常にそのまま
	// (Surround扱い)で後段へ渡す。5.1ch/7.1ch用の折り込み処理は
	// この22.2ch生データには対応していないため
	//
	// 「ダウンミックスしない」(m_LatmDownmixChannels == 0)の時は、
	// 空間オーディオ用レンダラ(24ch/48kHz/16bitしか受け付けない)へ
	// 出力する前提のため、22.2ch以外のデコーダ出力(通常のステレオ/
	// 5.1ch等。22.2chとは別のデコーダで処理される音声トラック)も
	// 24chの配置へ拡張して出力する。出力形式を24chのまま変えないことで、
	// 音声トラックを切り替えた時に、レンダラが受け付けられない形式への
	// 変更が起きない(受け付けられないと、レンダラのピンが実行時エラー
	// 状態になり、グラフを再初期化するまで無音になる)
	const bool Expand24 =
		(m_LatmDownmixChannels == 0)
		&& (Info.ChannelCount == 1 || Info.ChannelCount == 2
			|| Info.ChannelCount == 6 || Info.ChannelCount == 8);
	const bool Surround =
		((Info.ChannelCount == 6 || Info.ChannelCount == 8) && !m_DownMixSurround)
		|| (Info.ChannelCount == 24)
		|| Expand24;
	const int OutChannels = Expand24 ? 24 : (Surround ? Info.ChannelCount : 2);

	// メディアタイプの更新
	WAVEFORMATEX *pwfx = reinterpret_cast<WAVEFORMATEX *>(m_MediaType.Format());

	// m_MediaTypeは「次に提案する/期待する」形式で、設定変更等によって、
	// 実際に接続されている形式とは異なる値に書き換えられていることがある。
	// そのため、実際に接続されている形式とも比較する(異なっていれば、
	// 通常の形式変更として扱い、下流へ形式変更を通知する)
	bool ConnectedMismatch = false;
	if ((m_pOutput != nullptr) && m_pOutput->IsConnected()) {
		const CMediaType &Connected = m_pOutput->CurrentMediaType();
		if ((*Connected.FormatType() == FORMAT_WaveFormatEx)
				&& (Connected.FormatLength() >= sizeof(WAVEFORMATEX))) {
			const WAVEFORMATEX *pcwfx = reinterpret_cast<const WAVEFORMATEX *>(Connected.Format());
			ConnectedMismatch =
				(pcwfx->nChannels != OutChannels)
				|| (pcwfx->nSamplesPerSec != static_cast<DWORD>(Info.Frequency))
				|| (!Surround && (pcwfx->wFormatTag != WAVE_FORMAT_PCM))
				|| (Surround && (pcwfx->wFormatTag != WAVE_FORMAT_EXTENSIBLE));
		}
	}

	if ((*m_MediaType.FormatType() != FORMAT_WaveFormatEx)
			|| (!Surround && (pwfx->wFormatTag != WAVE_FORMAT_PCM))
			|| (Surround && (pwfx->wFormatTag != WAVE_FORMAT_EXTENSIBLE))
			|| (pwfx->nChannels != OutChannels)
			|| (pwfx->nSamplesPerSec != Info.Frequency)
			|| ConnectedMismatch) {
		CMediaType &mt = pSampleInfo->MediaType;
		mt.SetType(&MEDIATYPE_Audio);
		mt.SetSubtype(&MEDIASUBTYPE_PCM);
		mt.SetFormatType(&FORMAT_WaveFormatEx);

		pwfx = reinterpret_cast<WAVEFORMATEX *>(
			mt.AllocFormatBuffer(Surround ? sizeof (WAVEFORMATEXTENSIBLE) : sizeof(WAVEFORMATEX)));
		if (pwfx == nullptr)
			return E_OUTOFMEMORY;
		if (!Surround) {
			pwfx->wFormatTag = WAVE_FORMAT_PCM;
			pwfx->nChannels = 2;
			pwfx->cbSize = 0;
		} else {
			WAVEFORMATEXTENSIBLE *pExtensible = reinterpret_cast<WAVEFORMATEXTENSIBLE *>(pwfx);
			pExtensible->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
			pExtensible->Format.nChannels = static_cast<WORD>(OutChannels);
			pExtensible->Format.cbSize  = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
			if (OutChannels == 24) {
				// 22.2ch。Windows標準のdwChannelMaskには22.2ch全chぶんの
				// 定義(下層やLFE2等)が存在しないため、0(チャンネルマスク
				// 未指定。チャンネル順序のみで解釈する)を指定する。
				// 一般的なオーディオデバイスはこの24chをそのまま受け付け
				// られないため、実質的に22.2ch対応の専用レンダラ
				// (Windows空間オーディオ用等)を介する用途を想定している
				pExtensible->dwChannelMask = 0;
			} else if (OutChannels == 8) {
				// 7.1ch(FL,FR,FC,LFE,BL,BR,SL,SR)
				pExtensible->dwChannelMask =
					SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
					SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
					SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |
					SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
			} else {
				// 5.1ch(FL,FR,FC,LFE,BL,BR)
				pExtensible->dwChannelMask =
					SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
					SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
					SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
			}
			pExtensible->Samples.wValidBitsPerSample = 16;
			pExtensible->SubFormat = MEDIASUBTYPE_PCM;
		}
		pwfx->nSamplesPerSec = Info.Frequency;
		pwfx->wBitsPerSample = 16;
		pwfx->nBlockAlign = pwfx->nChannels * pwfx->wBitsPerSample / 8;
		pwfx->nAvgBytesPerSec = pwfx->nSamplesPerSec * pwfx->nBlockAlign;
		mt.SetSampleSize(pwfx->nBlockAlign);

		pSampleInfo->MediaTypeChanged = true;
		pSampleInfo->MediaBufferSize = SAMPLES_PER_FRAME * pwfx->nBlockAlign;
	}

	const size_t BuffSize = Samples * OutChannels * sizeof(int16_t);
	if (pSampleInfo->pData->SetSize(BuffSize) < BuffSize)
		return E_OUTOFMEMORY;
	uint8_t *pOutBuff = pSampleInfo->pData->GetData();

	// 24chへ拡張する場合の、拡張前(ステレオ/5.1ch/7.1ch)のチャンネル数
	int NarrowChannels = 0;

	if (pData != nullptr) {
		size_t OutSize = 0;

		if (Expand24) {
			// まず通常の経路(モノラル→ステレオ、二重音声/ステレオモードの
			// 選択、5.1chのチャンネル並び替えや折り込み等)で処理し、
			// その結果を24chの配置へ置く。24chのうち、元の音声に
			// 存在しないchは無音。
			const int16_t *pSrc = reinterpret_cast<const int16_t *>(pData);
			switch (Info.ChannelCount) {
			case 1:
			case 2:
				NarrowChannels = 2;
				break;
			case 6:
				NarrowChannels = m_DownMixSurround ? 2 : 6;
				break;
			case 8:
				NarrowChannels = m_DownMixSurround ? 2 : 8;
				break;
			}
			m_ExpandWork.resize(Samples * NarrowChannels);
			int16_t *pWork = m_ExpandWork.data();

			switch (Info.ChannelCount) {
			case 1:
				MonoToStereo(pWork, pSrc, Samples);
				break;
			case 2:
				DownMixStereo(pWork, pSrc, Samples);
				break;
			case 6:
				if (m_DownMixSurround)
					DownMixSurround(pWork, pSrc, Samples);
				else
					MapSurroundChannels(pWork, pSrc, Samples);
				break;
			case 8:
				if (m_DownMixSurround)
					DownMixSurround8(pWork, pSrc, Samples);
				else
					MapSurround8Channels(pWork, pSrc, Samples);
				break;
			}

			// 24chの並び(LatmSpatial::RawChannelOrder)の先頭6chは
			// FL,FR,FC,LFE,BL,BR、SL/SRは9,10番目
			static const int Slot8ch[8] = {0, 1, 2, 3, 4, 5, 9, 10};
			int16_t *pDst = reinterpret_cast<int16_t *>(pOutBuff);
			std::memset(pDst, 0, BuffSize);
			for (size_t i = 0; i < Samples; i++) {
				for (int c = 0; c < NarrowChannels; c++) {
					const int Slot = (NarrowChannels == 8) ? Slot8ch[c] : c;
					pDst[i * 24 + Slot] = pWork[i * NarrowChannels + c];
				}
			}
			OutSize = Samples * 24 * sizeof(int16_t);
		} else

		// ダウンミックス
		switch (Info.ChannelCount) {
		case 1:
			OutSize = MonoToStereo(
				reinterpret_cast<int16_t *>(pOutBuff),
				reinterpret_cast<const int16_t *>(pData),
				Samples);
			break;

		case 2:
			OutSize = DownMixStereo(
				reinterpret_cast<int16_t *>(pOutBuff),
				reinterpret_cast<const int16_t *>(pData),
				Samples);
			break;

		case 6:
			if (Surround) {
				OutSize = MapSurroundChannels(
					reinterpret_cast<int16_t *>(pOutBuff),
					reinterpret_cast<const int16_t *>(pData),
					Samples);
			} else {
				OutSize = DownMixSurround(
					reinterpret_cast<int16_t *>(pOutBuff),
					reinterpret_cast<const int16_t *>(pData),
					Samples);
			}
			break;

		case 8:
			// 22.2ch等をデコーダ側(AACLATMDecoder_FDK)で7.1ch(8ch)まで
			// 落とし込んだ結果。ここでは更に5.1ch用のチャンネルマップや
			// ダウンミックス行列は流用せず、7.1ch専用の経路を通す
			if (Surround) {
				OutSize = MapSurround8Channels(
					reinterpret_cast<int16_t *>(pOutBuff),
					reinterpret_cast<const int16_t *>(pData),
					Samples);
			} else {
				OutSize = DownMixSurround8(
					reinterpret_cast<int16_t *>(pOutBuff),
					reinterpret_cast<const int16_t *>(pData),
					Samples);
			}
			break;

		case 24:
			// 22.2ch。デコーダ側(AACLATMDecoder_Libav)が既に標準順序の
			// 24chインターリーブPCMを生成しているため、ここでは折り込み・
			// 並び替えを行わず、そのままコピーするだけで良い
			// (OutSizeは他のcaseと同じく「バイト数」で返す約束になっている)
			OutSize = Samples * 24 * sizeof(int16_t);
			std::memcpy(pOutBuff, pData, OutSize);
			break;
		}

		if (m_GainControl && (Info.ChannelCount < 6 || Surround)) {
			const bool UseSurroundGain = Expand24 ? (NarrowChannels > 2) : Surround;
			GainControl(
				reinterpret_cast<int16_t *>(pOutBuff), OutSize / sizeof(int16_t),
				UseSurroundGain ? m_SurroundGain : m_Gain);
		}
	} else {
		std::memset(pOutBuff, 0, BuffSize);
	}

	m_OutChannelNum = static_cast<uint8_t>(OutChannels);

	if (m_pSampleCallback) {
		m_pSampleCallback->OnSamples(reinterpret_cast<int16_t *>(pOutBuff), Samples, OutChannels);
	}

	pSampleInfo->SampleCount = Samples;

	return S_OK;
}


HRESULT AudioDecoderFilter::ProcessSPDIF(
	const AudioDecoder::AudioInfo &Info, FrameSampleInfo *pSampleInfo)
{
	constexpr int PREAMBLE_SIZE = sizeof(WORD) * 4;

	AudioDecoder::SPDIFFrameInfo FrameInfo;
	if (!m_Decoder->GetSPDIFFrameInfo(&FrameInfo))
		return E_FAIL;

	const int FrameSize = FrameInfo.FrameSize;
	const int DataBurstSize = PREAMBLE_SIZE + FrameSize;
	const int PacketSize = FrameInfo.SamplesPerFrame * 4;
	if (DataBurstSize > PacketSize) {
		LIBISDB_TRACE(
			LIBISDB_STR("S/PDIFビットレートが不正です。(Frame size {} / Data-burst size {} / Packet size {})\n"),
			FrameSize, DataBurstSize, PacketSize);
		return E_FAIL;
	}

#ifdef LIBISDB_DEBUG
	static bool First = true;
	if (First) {
		LIBISDB_TRACE(
			LIBISDB_STR("S/PDIF出力開始(Frame size {} / Data-burst size {} / Packet size {})\n"),
			FrameSize, DataBurstSize, PacketSize);
		First = false;
	}
#endif

	WAVEFORMATEX *pwfx = reinterpret_cast<WAVEFORMATEX*>(m_MediaType.Format());
	if ((*m_MediaType.FormatType() != FORMAT_WaveFormatEx)
			|| (pwfx->wFormatTag != WAVE_FORMAT_DOLBY_AC3_SPDIF)
			|| (pwfx->nSamplesPerSec != Info.Frequency)) {
		CMediaType &mt = pSampleInfo->MediaType;
		mt.SetType(&MEDIATYPE_Audio);
		mt.SetSubtype(&MEDIASUBTYPE_PCM);
		mt.SetFormatType(&FORMAT_WaveFormatEx);

		pwfx = reinterpret_cast<WAVEFORMATEX*>(mt.AllocFormatBuffer(sizeof(WAVEFORMATEX)));
		if (pwfx == nullptr)
			return E_OUTOFMEMORY;
		pwfx->wFormatTag = WAVE_FORMAT_DOLBY_AC3_SPDIF;
		pwfx->nChannels = 2;
		pwfx->nSamplesPerSec = Info.Frequency;
		pwfx->wBitsPerSample = 16;
		pwfx->nBlockAlign = pwfx->nChannels * pwfx->wBitsPerSample / 8;
		pwfx->nAvgBytesPerSec = pwfx->nSamplesPerSec * pwfx->nBlockAlign;
		pwfx->cbSize = 0;
		/*
		// Windows 7 では WAVEFORMATEXTENSIBLE_IEC61937 を使った方がいい?
		WAVEFORMATEXTENSIBLE_IEC61937 *pwfx;
		...
		pwfx->FormatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		...
		pwfx->FormatExt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE_IEC61937) - sizeof(WAVEFORMATEX);
		pwfx->FormatExt.dwChannelMask =
			SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
			SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
			SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
		pwfx->FormatExt.Samples.wValidBitsPerSample = 16;
		pwfx->FormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_IEC61937_AAC;
		pwfx->dwEncodedSamplesPerSec = Info.Frequency;
		pwfx->dwEncodedChannelCount = 6;
		pwfx->dwAverageBytesPerSec = 0;
		*/
		mt.SetSampleSize(pwfx->nBlockAlign);

		pSampleInfo->MediaTypeChanged = true;
		pSampleInfo->MediaBufferSize = PacketSize;
	}

	if (pSampleInfo->pData->SetSize(PacketSize) < static_cast<DWORD>(PacketSize))
		return E_OUTOFMEMORY;
	BYTE *pOutBuff = pSampleInfo->pData->GetData();
	WORD *pWordData = reinterpret_cast<WORD*>(pOutBuff);
	// Burst-preamble
	pWordData[0] = 0xF872;        // Pa(Sync word 1)
	pWordData[1] = 0x4E1F;        // Pb(Sync word 2)
	pWordData[2] = FrameInfo.Pc;  // Pc(Burst-info)
	//pWordData[3] = ((FrameSize + 1) & ~1) * 8; // Pd(Length-code)
	pWordData[3] = FrameSize * 8; // Pd(Length-code)
	// Burst-payload
	int PayloadSize = m_Decoder->GetSPDIFBurstPayload(
			&pOutBuff[PREAMBLE_SIZE],
			pSampleInfo->pData->GetBufferSize() - PREAMBLE_SIZE);
	if ((PayloadSize < 1) || (PREAMBLE_SIZE + PayloadSize > PacketSize)) {
		LIBISDB_TRACE(
			LIBISDB_STR("S/PDIF Burst-payload サイズが不正です。(Packet size {} / Payload size {})\n"),
			PacketSize, PayloadSize);
		return E_FAIL;
	}
	// Stuffing
	PayloadSize += PREAMBLE_SIZE;
	if (PayloadSize < PacketSize)
		::ZeroMemory(&pOutBuff[PayloadSize], PacketSize - PayloadSize);

	pSampleInfo->SampleCount = FrameInfo.SamplesPerFrame;

	return S_OK;
}


HRESULT AudioDecoderFilter::ReconnectOutput(long BufferSize, const CMediaType &mt)
{
	HRESULT hr;

	IPin *pPin = m_pOutput->GetConnected();
	if (pPin == nullptr)
		return E_POINTER;

	IMemInputPin *pMemInputPin = nullptr;
	hr = pPin->QueryInterface(IID_IMemInputPin, reinterpret_cast<void**>(&pMemInputPin));
	if (FAILED(hr)) {
		LIBISDB_TRACE(LIBISDB_STR("IMemInputPinインターフェースが取得できません。({:08x})\n"), hr);
	} else {
		IMemAllocator *pAllocator = nullptr;
		hr = pMemInputPin->GetAllocator(&pAllocator);
		if (FAILED(hr)) {
			LIBISDB_TRACE(LIBISDB_STR("IMemAllocatorインターフェースが取得できません。({:08x})\n"), hr);
		} else {
			ALLOCATOR_PROPERTIES Props;
			hr = pAllocator->GetProperties(&Props);
			if (FAILED(hr)) {
				LIBISDB_TRACE(LIBISDB_STR("IMemAllocatorのプロパティが取得できません。({:08x})\n"), hr);
			} else {
				if ((mt != m_pOutput->CurrentMediaType())
						|| (Props.cBuffers < NUM_SAMPLE_BUFFERS)
						|| (Props.cbBuffer < BufferSize)) {
					hr = S_OK;
					if ((Props.cBuffers < NUM_SAMPLE_BUFFERS)
							|| (Props.cbBuffer < BufferSize)) {
						ALLOCATOR_PROPERTIES ActualProps;

						Props.cBuffers = NUM_SAMPLE_BUFFERS;
						Props.cbBuffer = BufferSize * 3 / 2;
						LIBISDB_TRACE(LIBISDB_STR("バッファサイズを設定します。({} bytes)\n"), Props.cbBuffer);
						if (SUCCEEDED(hr = m_pOutput->DeliverBeginFlush())
								&& SUCCEEDED(hr = m_pOutput->DeliverEndFlush())
								&& SUCCEEDED(hr = pAllocator->Decommit())
								&& SUCCEEDED(hr = pAllocator->SetProperties(&Props, &ActualProps))
								&& SUCCEEDED(hr = pAllocator->Commit())) {
							if ((ActualProps.cBuffers < Props.cBuffers)
									|| (ActualProps.cbBuffer < BufferSize)) {
								LIBISDB_TRACE(LIBISDB_STR("バッファサイズの要求が受け付けられません。({} / {})\n"),
										  ActualProps.cbBuffer, Props.cbBuffer);
								hr = E_FAIL;
								NotifyEvent(EC_ERRORABORT, hr, 0);
							} else {
								LIBISDB_TRACE(LIBISDB_STR("ピンの再接続成功\n"));
								hr = S_OK;
							}
						} else {
							LIBISDB_TRACE(LIBISDB_STR("ピンの再接続ができません。({:08x})\n"), hr);
						}
					}
				} else {
					hr = S_FALSE;
				}
			}

			pAllocator->Release();
		}

		pMemInputPin->Release();
	}

	return hr;
}


void AudioDecoderFilter::ResetSync()
{
	m_DelayAdjustment = 0;
	m_StartTime = -1;
	m_SampleCount = 0;
	m_Discontinuity = true;
	m_InputDiscontinuity = true;
}


size_t AudioDecoderFilter::MonoToStereo(int16_t *pDst, const int16_t *pSrc, size_t Samples)
{
	// 1ch → 2ch 二重化
	const int16_t *p = pSrc, *pEnd = pSrc + Samples;
	int16_t *q = pDst;

#ifdef LIBISDB_SSE2_SUPPORT
	if (Samples >= 16 && IsSSE2Enabled()) {
		const int16_t *pSimdEnd = pSrc + (Samples & ~15_z);
		do {
			__m128i v1, v2, r1, r2, r3, r4;
			v1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p) + 0);
			v2 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p) + 1);
			r1 = _mm_unpacklo_epi16(v1, v1);
			r2 = _mm_unpackhi_epi16(v1, v1);
			r3 = _mm_unpacklo_epi16(v2, v2);
			r4 = _mm_unpackhi_epi16(v2, v2);
			_mm_store_si128(reinterpret_cast<__m128i *>(q) + 0, r1);
			_mm_store_si128(reinterpret_cast<__m128i *>(q) + 1, r2);
			_mm_store_si128(reinterpret_cast<__m128i *>(q) + 2, r3);
			_mm_store_si128(reinterpret_cast<__m128i *>(q) + 3, r4);
			q += 32;
			p += 16;
		} while (p < pSimdEnd);
	}
#endif

	while (p < pEnd) {
		int16_t Value = *p++;
		*q++ = Value; // L
		*q++ = Value; // R
	}

	return Samples * (sizeof(int16_t) * 2);
}


size_t AudioDecoderFilter::DownMixStereo(int16_t *pDst, const int16_t *pSrc, size_t Samples)
{
	if (m_StereoMode == StereoMode::Stereo) {
		// 2ch → 2ch スルー
		std::memcpy(pDst, pSrc, Samples * (sizeof(int16_t) * 2));
	} else {
		const size_t Length = Samples * 2;
		const int16_t *p = pSrc, *pEnd = pSrc + Length;
		int16_t *q = pDst;

		if (m_StereoMode == StereoMode::Left) {
			// 左のみ

#ifdef LIBISDB_SSE2_SUPPORT
			if (Length >= 32 && IsSSE2Enabled()) {
				const int16_t *pSimdEnd = pSrc + (Length & ~31_z);
				do {
					__m128i v1, v2, v3, v4;
					v1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p) + 0);
					v2 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p) + 1);
					v3 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p) + 2);
					v4 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p) + 3);
					v1 = _mm_shufflelo_epi16(v1, _MM_SHUFFLE(2, 2, 0, 0));
					v2 = _mm_shufflelo_epi16(v2, _MM_SHUFFLE(2, 2, 0, 0));
					v3 = _mm_shufflelo_epi16(v3, _MM_SHUFFLE(2, 2, 0, 0));
					v4 = _mm_shufflelo_epi16(v4, _MM_SHUFFLE(2, 2, 0, 0));
					v1 = _mm_shufflehi_epi16(v1, _MM_SHUFFLE(2, 2, 0, 0));
					v2 = _mm_shufflehi_epi16(v2, _MM_SHUFFLE(2, 2, 0, 0));
					v3 = _mm_shufflehi_epi16(v3, _MM_SHUFFLE(2, 2, 0, 0));
					v4 = _mm_shufflehi_epi16(v4, _MM_SHUFFLE(2, 2, 0, 0));
					_mm_store_si128(reinterpret_cast<__m128i *>(q) + 0, v1);
					_mm_store_si128(reinterpret_cast<__m128i *>(q) + 1, v2);
					_mm_store_si128(reinterpret_cast<__m128i *>(q) + 2, v3);
					_mm_store_si128(reinterpret_cast<__m128i *>(q) + 3, v4);
					q += 32;
					p += 32;
				} while (p < pSimdEnd);
			}
#endif

			while (p < pEnd) {
				int16_t Value = *p;
				*q++ = Value; // L
				*q++ = Value; // R
				p += 2;
			}
		} else {
			// 右のみ

#ifdef LIBISDB_SSE2_SUPPORT
			if (Length >= 32 && IsSSE2Enabled()) {
				const int16_t *pSimdEnd = pSrc + (Length & ~31_z);
				do {
					__m128i v1, v2, v3, v4;
					v1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p) + 0);
					v2 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p) + 1);
					v3 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p) + 2);
					v4 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p) + 3);
					v1 = _mm_shufflelo_epi16(v1, _MM_SHUFFLE(3, 3, 1, 1));
					v2 = _mm_shufflelo_epi16(v2, _MM_SHUFFLE(3, 3, 1, 1));
					v3 = _mm_shufflelo_epi16(v3, _MM_SHUFFLE(3, 3, 1, 1));
					v4 = _mm_shufflelo_epi16(v4, _MM_SHUFFLE(3, 3, 1, 1));
					v1 = _mm_shufflehi_epi16(v1, _MM_SHUFFLE(3, 3, 1, 1));
					v2 = _mm_shufflehi_epi16(v2, _MM_SHUFFLE(3, 3, 1, 1));
					v3 = _mm_shufflehi_epi16(v3, _MM_SHUFFLE(3, 3, 1, 1));
					v4 = _mm_shufflehi_epi16(v4, _MM_SHUFFLE(3, 3, 1, 1));
					_mm_store_si128(reinterpret_cast<__m128i *>(q) + 0, v1);
					_mm_store_si128(reinterpret_cast<__m128i *>(q) + 1, v2);
					_mm_store_si128(reinterpret_cast<__m128i *>(q) + 2, v3);
					_mm_store_si128(reinterpret_cast<__m128i *>(q) + 3, v4);
					q += 32;
					p += 32;
				} while (p < pSimdEnd);
			}
#endif

			while (p < pEnd) {
				int16_t Value = p[1];
				*q++ = Value; // L
				*q++ = Value; // R
				p += 2;
			}
		}
	}

	return Samples * (sizeof(int16_t) * 2);
}


size_t AudioDecoderFilter::DownMixSurround(int16_t *pDst, const int16_t *pSrc, size_t Samples)
{
	// 5.1ch → 2ch ダウンミックス

	const double Level = m_GainControl ? m_SurroundGain : 1.0;
	int ChannelMap[6];

	if (!m_Decoder->GetChannelMap(6, ChannelMap)) {
		for (int i = 0; i < 6; i++)
			ChannelMap[i] = i;
	}

	if (m_EnableCustomDownMixMatrix) {
		// カスタムマトリックスを使用
		for (size_t Pos = 0; Pos < Samples; Pos++) {
			double Data[6];

			for (int i = 0; i < 6; i++)
				Data[i] = static_cast<double>(pSrc[Pos * 6 + ChannelMap[i]]);

			for (int i = 0; i < 2; i++) {
				const int Value = static_cast<int>((
					Data[0] * m_DownMixMatrix.Matrix[i][0] +
					Data[1] * m_DownMixMatrix.Matrix[i][1] +
					Data[2] * m_DownMixMatrix.Matrix[i][2] +
					Data[3] * m_DownMixMatrix.Matrix[i][3] +
					Data[4] * m_DownMixMatrix.Matrix[i][4] +
					Data[5] * m_DownMixMatrix.Matrix[i][5]
					) * Level);
				pDst[Pos * 2 + i] = ClampSample16(Value);
			}
		}
	} else {
		// デフォルトの係数を使用
		AudioDecoder::DownmixInfo Info;
		m_Decoder->GetDownmixInfo(&Info);

		for (size_t Pos = 0; Pos < Samples; Pos++) {
			const int Left = static_cast<int>((
				static_cast<double>(pSrc[Pos * 6 + ChannelMap[AudioDecoder::CHANNEL_6_FL ]]) * Info.Front +
				static_cast<double>(pSrc[Pos * 6 + ChannelMap[AudioDecoder::CHANNEL_6_BL ]]) * Info.Rear +
				static_cast<double>(pSrc[Pos * 6 + ChannelMap[AudioDecoder::CHANNEL_6_FC ]]) * Info.Center +
				static_cast<double>(pSrc[Pos * 6 + ChannelMap[AudioDecoder::CHANNEL_6_LFE]]) * Info.LFE
				) * Level);

			const int Right = static_cast<int>((
				static_cast<double>(pSrc[Pos * 6 + ChannelMap[AudioDecoder::CHANNEL_6_FR ]]) * Info.Front +
				static_cast<double>(pSrc[Pos * 6 + ChannelMap[AudioDecoder::CHANNEL_6_BR ]]) * Info.Rear +
				static_cast<double>(pSrc[Pos * 6 + ChannelMap[AudioDecoder::CHANNEL_6_FC ]]) * Info.Center +
				static_cast<double>(pSrc[Pos * 6 + ChannelMap[AudioDecoder::CHANNEL_6_LFE]]) * Info.LFE
				) * Level);

			pDst[Pos * 2 + 0] = ClampSample16(Left);
			pDst[Pos * 2 + 1] = ClampSample16(Right);
		}
	}

	// バッファサイズを返す
	return Samples * (sizeof(int16_t) * 2);
}


size_t AudioDecoderFilter::MapSurroundChannels(int16_t *pDst, const int16_t *pSrc, size_t Samples)
{
	if (m_EnableCustomMixingMatrix) {
		// カスタムマトリックスを使用
		int ChannelMap[6];

		if (!m_Decoder->GetChannelMap(6, ChannelMap)) {
			for (int i = 0; i < 6; i++)
				ChannelMap[i] = i;
		}

		for (size_t i = 0 ; i < Samples ; i++) {
			double Data[6];

			for (int j = 0; j < 6; j++)
				Data[j] = static_cast<double>(pSrc[i * 6 + ChannelMap[j]]);

			for (int j = 0; j < 6; j++) {
				const int Value = static_cast<int>(
					Data[0] * m_MixingMatrix.Matrix[j][0] +
					Data[1] * m_MixingMatrix.Matrix[j][1] +
					Data[2] * m_MixingMatrix.Matrix[j][2] +
					Data[3] * m_MixingMatrix.Matrix[j][3] +
					Data[4] * m_MixingMatrix.Matrix[j][4] +
					Data[5] * m_MixingMatrix.Matrix[j][5]);
				pDst[i * 6 + j] = ClampSample16(Value);
			}
		}
	} else {
		// デフォルトの割り当てを使用
		int ChannelMap[6];

		if (m_Decoder->GetChannelMap(6, ChannelMap)) {
			for (size_t i = 0 ; i < Samples ; i++) {
				pDst[i * 6 + 0] = pSrc[i * 6 + ChannelMap[AudioDecoder::CHANNEL_6_FL]];
				pDst[i * 6 + 1] = pSrc[i * 6 + ChannelMap[AudioDecoder::CHANNEL_6_FR]];
				pDst[i * 6 + 2] = pSrc[i * 6 + ChannelMap[AudioDecoder::CHANNEL_6_FC]];
				pDst[i * 6 + 3] = pSrc[i * 6 + ChannelMap[AudioDecoder::CHANNEL_6_LFE]];
				pDst[i * 6 + 4] = pSrc[i * 6 + ChannelMap[AudioDecoder::CHANNEL_6_BL]];
				pDst[i * 6 + 5] = pSrc[i * 6 + ChannelMap[AudioDecoder::CHANNEL_6_BR]];
			}
		} else {
			std::memcpy(pDst, pSrc, Samples * 6 * sizeof(int16_t));
		}
	}

	return Samples * (sizeof(int16_t) * 6);
}


size_t AudioDecoderFilter::MapSurround8Channels(int16_t *pDst, const int16_t *pSrc, size_t Samples)
{
	// 7.1ch(8ch)のチャンネル並び替え。22.2ch等をデコーダ側(AACLATMDecoder_FDK)
	// で7.1chへダウンミックスした結果を、出力用の順序(FL,FR,FC,LFE,BL,BR,SL,SR)
	// へ揃える。5.1ch用のカスタムマトリックス(m_MixingMatrix)は6ch専用なので
	// ここでは流用せず、常にデフォルトの並び替えのみを行う
	int ChannelMap[8];

	if (m_Decoder->GetChannelMap(8, ChannelMap)) {
		for (size_t i = 0 ; i < Samples ; i++) {
			pDst[i * 8 + 0] = pSrc[i * 8 + ChannelMap[AudioDecoder::CHANNEL_8_FL]];
			pDst[i * 8 + 1] = pSrc[i * 8 + ChannelMap[AudioDecoder::CHANNEL_8_FR]];
			pDst[i * 8 + 2] = pSrc[i * 8 + ChannelMap[AudioDecoder::CHANNEL_8_FC]];
			pDst[i * 8 + 3] = pSrc[i * 8 + ChannelMap[AudioDecoder::CHANNEL_8_LFE]];
			pDst[i * 8 + 4] = pSrc[i * 8 + ChannelMap[AudioDecoder::CHANNEL_8_BL]];
			pDst[i * 8 + 5] = pSrc[i * 8 + ChannelMap[AudioDecoder::CHANNEL_8_BR]];
			pDst[i * 8 + 6] = pSrc[i * 8 + ChannelMap[AudioDecoder::CHANNEL_8_SL]];
			pDst[i * 8 + 7] = pSrc[i * 8 + ChannelMap[AudioDecoder::CHANNEL_8_SR]];
		}
	} else {
		std::memcpy(pDst, pSrc, Samples * 8 * sizeof(int16_t));
	}

	return Samples * (sizeof(int16_t) * 8);
}


size_t AudioDecoderFilter::DownMixSurround8(int16_t *pDst, const int16_t *pSrc, size_t Samples)
{
	// 7.1ch(8ch) → 2ch ダウンミックス
	// (「サラウンドをダウンミックスする」設定が有効な場合に使用)

	const double Level = m_GainControl ? m_SurroundGain : 1.0;
	int ChannelMap[8];

	if (!m_Decoder->GetChannelMap(8, ChannelMap)) {
		for (int i = 0; i < 8; i++)
			ChannelMap[i] = i;
	}

	AudioDecoder::DownmixInfo Info;
	m_Decoder->GetDownmixInfo(&Info);

	// サイドチャンネルはリアと同じ係数で扱う
	for (size_t Pos = 0; Pos < Samples; Pos++) {
		const int Left = static_cast<int>((
			static_cast<double>(pSrc[Pos * 8 + ChannelMap[AudioDecoder::CHANNEL_8_FL ]]) * Info.Front +
			static_cast<double>(pSrc[Pos * 8 + ChannelMap[AudioDecoder::CHANNEL_8_BL ]]) * Info.Rear +
			static_cast<double>(pSrc[Pos * 8 + ChannelMap[AudioDecoder::CHANNEL_8_SL ]]) * Info.Rear +
			static_cast<double>(pSrc[Pos * 8 + ChannelMap[AudioDecoder::CHANNEL_8_FC ]]) * Info.Center +
			static_cast<double>(pSrc[Pos * 8 + ChannelMap[AudioDecoder::CHANNEL_8_LFE]]) * Info.LFE
			) * Level);

		const int Right = static_cast<int>((
			static_cast<double>(pSrc[Pos * 8 + ChannelMap[AudioDecoder::CHANNEL_8_FR ]]) * Info.Front +
			static_cast<double>(pSrc[Pos * 8 + ChannelMap[AudioDecoder::CHANNEL_8_BR ]]) * Info.Rear +
			static_cast<double>(pSrc[Pos * 8 + ChannelMap[AudioDecoder::CHANNEL_8_SR ]]) * Info.Rear +
			static_cast<double>(pSrc[Pos * 8 + ChannelMap[AudioDecoder::CHANNEL_8_FC ]]) * Info.Center +
			static_cast<double>(pSrc[Pos * 8 + ChannelMap[AudioDecoder::CHANNEL_8_LFE]]) * Info.LFE
			) * Level);

		pDst[Pos * 2 + 0] = ClampSample16(Left);
		pDst[Pos * 2 + 1] = ClampSample16(Right);
	}

	return Samples * (sizeof(int16_t) * 2);
}


void AudioDecoderFilter::GainControl(int16_t *pBuffer, size_t Samples, float Gain)
{
	constexpr int Factor = 0x1000;
	const int Level = static_cast<int>(Gain * static_cast<float>(Factor));

	if (Level != Factor) {
		int16_t *p, *pEnd;

		p = pBuffer;
		pEnd= p + Samples;
		while (p < pEnd) {
			const int Value = (static_cast<int>(*p) * Level) / Factor;
			*p++ = ClampSample16(Value);
		}
	}
}


void AudioDecoderFilter::SelectDualMonoStereoMode()
{
	switch (m_DualMonoMode) {
	case DualMonoMode::Main: m_StereoMode = StereoMode::Left;   break;
	case DualMonoMode::Sub:  m_StereoMode = StereoMode::Right;  break;
	case DualMonoMode::Both: m_StereoMode = StereoMode::Stereo; break;
	}
}


AudioDecoder * AudioDecoderFilter::CreateDecoder(DecoderType Type)
{
	switch (Type) {
	case DecoderType::AAC:
		return new AACDecoder_FAAD2;

	case DecoderType::MPEGAudio:
		return new MPEGAudioDecoder;

	case DecoderType::AC3:
		return new AC3Decoder;

#ifdef LIBISDB_HAS_FDK_AAC
	case DecoderType::FDK_AAC:
		return new AACDecoder_FDK;

	case DecoderType::AAC_LATM:
		return new AACLATMDecoder_FDK;
#endif

#ifdef LIBISDB_HAS_LIBAV_AAC
	case DecoderType::AAC_LATM_LIBAV:
		return new AACLATMDecoder_Libav;
#endif

	}

	return nullptr;
}


bool AudioDecoderFilter::GetDecoderVersion(DecoderType Type, std::string *pVersion)
{
	if (pVersion == nullptr)
		return false;

	switch (Type) {
	case DecoderType::AAC:
		AACDecoder_FAAD2::GetVersion(pVersion);
		return true;

	case DecoderType::MPEGAudio:
		MPEGAudioDecoder::GetVersion(pVersion);
		return true;

	case DecoderType::AC3:
		AC3Decoder::GetVersion(pVersion);
		return true;

#ifdef LIBISDB_HAS_FDK_AAC
	case DecoderType::FDK_AAC:
		AACDecoder_FDK::GetVersion(pVersion);
		return true;

	case DecoderType::AAC_LATM:
		AACDecoder_FDK::GetVersion(pVersion);
		return true;
#endif

#ifdef LIBISDB_HAS_LIBAV_AAC
	case DecoderType::AAC_LATM_LIBAV:
		AACLATMDecoder_Libav::GetVersion(pVersion);
		return true;
#endif
	}

	pVersion->clear();

	return false;
}


} // namespace LibISDB::DirectShow
