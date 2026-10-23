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
 @file   SpatialAudioRenderer.cpp
 @brief  Windows空間オーディオ(ISpatialAudioClient)を使う、22.2ch用の
         DirectShowレンダラフィルタ
 @author DBCTRADO

 【重要】このファイルはWindows SDKの spatialaudioclient.h と首っ引きで
 実装したものですが、実機でのビルド確認は行えていません。特に
 - ActivateSpatialAudioStream() へ渡すPROPVARIANTの組み立て方
 - SpatialAudioObjectRenderStreamActivationParams のメンバ名・順序
 - BeginUpdatingAudioObjects()/EndUpdatingAudioObjects() の呼び出し規約
 あたりは、実際のSDKヘッダ(spatialaudioclient.h)と照らし合わせての
 修正が必要になる可能性が高いです。コンパイルエラーが出た場合は、
 エラーメッセージ(特に構造体メンバ名の不一致)を教えてください。
*/


#include "../../../../LibISDBPrivate.hpp"
#include "../../../../LibISDBWindows.hpp"
#include "SpatialAudioRenderer.hpp"
#include "../../../../Base/DebugDef.hpp"

#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <algorithm>
#include <avrt.h>
#include <cwchar>

#pragma comment(lib, "avrt.lib")

using namespace Microsoft::WRL;


namespace LibISDB::DirectShow
{


namespace
{

	// {8C1E7B2A-4F3D-4A6E-9B1C-2E6D5F7A9C10} (仮。実際に使う際は
	// uuidgen.exe / guidgen.exe で生成したGUIDに差し替えること)
	// clang-format off
	static const GUID CLSID_SpatialAudioRenderer = {
		0x8c1e7b2a, 0x4f3d, 0x4a6e, {0x9b, 0x1c, 0x2e, 0x6d, 0x5f, 0x7a, 0x9c, 0x10}
	};
	// clang-format on

	// 動的オブジェクト用フォーマット(モノラル, float32, 48kHz)
	WAVEFORMATEX MakeObjectFormat()
	{
		WAVEFORMATEX fmt{};
		fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
		fmt.nChannels = 1;
		fmt.nSamplesPerSec = SpatialAudioRenderer::SOURCE_SAMPLE_RATE;
		fmt.wBitsPerSample = 32;
		fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
		fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
		fmt.cbSize = 0;
		return fmt;
	}

}	// namespace


SpatialAudioRenderer::SpatialAudioRenderer(LPUNKNOWN pUnk, HRESULT *phr)
	: CBaseRenderer(CLSID_SpatialAudioRenderer, TEXT("22.2ch Spatial Audio Renderer"), pUnk, phr)
	, m_hBufferCompletionEvent(nullptr)
	, m_RingBufferSamples(0)
	, m_RingWritePos(0)
	, m_RingReadPos(0)
	, m_Buffering(true)
	, m_ReadFrac(0.0)
	, m_RateAdjust(1.0)
	, m_Initialized(false)
{
	m_StopRenderThread = false;
	m_Streaming = false;
	::InitializeCriticalSection(&m_RingLock);
}


SpatialAudioRenderer::~SpatialAudioRenderer()
{
	FinalizeSpatialAudio();
	::DeleteCriticalSection(&m_RingLock);
}


bool SpatialAudioRenderer::IsAvailable()
{
	ComPtr<IMMDeviceEnumerator> pEnumerator;
	HRESULT hr = ::CoCreateInstance(
		__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
		IID_PPV_ARGS(&pEnumerator));
	if (FAILED(hr))
		return false;

	ComPtr<IMMDevice> pDevice;
	hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
	if (FAILED(hr))
		return false;

	ComPtr<ISpatialAudioClient> pClient;
	hr = pDevice->Activate(__uuidof(ISpatialAudioClient), CLSCTX_ALL, nullptr, &pClient);
	if (FAILED(hr))
		return false;

	// 対象フォーマット(モノラル/float32/48kHz)がオブジェクトとして
	// サポートされているか確認する
	const WAVEFORMATEX Format = MakeObjectFormat();
	BOOL FormatSupported = FALSE;
	hr = pClient->IsAudioObjectFormatSupported(&Format);
	if (FAILED(hr))
		return false;

	UINT32 MaxDynamicObjectCount = 0;
	hr = pClient->GetMaxDynamicObjectCount(&MaxDynamicObjectCount);
	if (FAILED(hr))
		return false;

	LIBISDB_TRACE(
		LIBISDB_STR("SpatialAudioRenderer::IsAvailable() MaxDynamicObjectCount={}\n"),
		MaxDynamicObjectCount);

	return MaxDynamicObjectCount >= static_cast<UINT32>(SpatialAudioRenderer::OBJECT_COUNT);
}


HRESULT SpatialAudioRenderer::CheckMediaType(const CMediaType *pmt)
{
	// 診断用: このフィルタがDirectShowのグラフ構築時に
	// そもそも「候補」として呼ばれているかどうかを必ず出力する
	::OutputDebugStringW(L"[LatmSpatial] SpatialAudioRenderer::CheckMediaType() called\n");

	if (pmt == nullptr)
		return E_POINTER;

	// 診断用: 提案されたメディアタイプの中身(major/subtype一致・
	// フォーマット詳細)を出力する。「なぜ拒否されるか」を切り分けるため
	{
		const bool TypeOK = (*pmt->Type() == MEDIATYPE_Audio) && (*pmt->Subtype() == MEDIASUBTYPE_PCM);
		const bool FormatTypeOK = (*pmt->FormatType() == FORMAT_WaveFormatEx);
		const WAVEFORMATEX *pDiagWfx =
			FormatTypeOK ? reinterpret_cast<const WAVEFORMATEX *>(pmt->Format()) : nullptr;

		wchar_t szBuff[256];
		if (pDiagWfx != nullptr) {
			swprintf_s(
				szBuff,
				L"[LatmSpatial]   TypeOK=%d FormatTypeOK=%d nChannels=%u nSamplesPerSec=%lu wBitsPerSample=%u wFormatTag=%u\n",
				TypeOK, FormatTypeOK, pDiagWfx->nChannels, pDiagWfx->nSamplesPerSec,
				pDiagWfx->wBitsPerSample, pDiagWfx->wFormatTag);
		} else {
			swprintf_s(
				szBuff, L"[LatmSpatial]   TypeOK=%d FormatTypeOK=%d (フォーマットデータなし)\n",
				TypeOK, FormatTypeOK);
		}
		::OutputDebugStringW(szBuff);
	}

	if (pmt == nullptr)
		return E_POINTER;

	if ((*pmt->Type() != MEDIATYPE_Audio) || (*pmt->Subtype() != MEDIASUBTYPE_PCM))
		return VFW_E_TYPE_NOT_ACCEPTED;

	if (*pmt->FormatType() != FORMAT_WaveFormatEx)
		return VFW_E_TYPE_NOT_ACCEPTED;

	const WAVEFORMATEX *pwfx = reinterpret_cast<const WAVEFORMATEX *>(pmt->Format());
	if (pwfx == nullptr)
		return VFW_E_TYPE_NOT_ACCEPTED;

	if ((pwfx->nChannels != SOURCE_CHANNELS)
			|| (pwfx->nSamplesPerSec != SOURCE_SAMPLE_RATE)
			|| (pwfx->wBitsPerSample != SOURCE_BITS))
		return VFW_E_TYPE_NOT_ACCEPTED;

	// 環境が対応していなければここで拒否する
	// (毎回デバイスに問い合わせるのはやや重いが、接続時にしか呼ばれない)
	if (!IsAvailable())
		return VFW_E_TYPE_NOT_ACCEPTED;

	return S_OK;
}


HRESULT SpatialAudioRenderer::SetMediaType(const CMediaType *pmt)
{
	const HRESULT hr = CBaseRenderer::SetMediaType(pmt);
	if (FAILED(hr))
		return hr;

	if (!InitializeSpatialAudio())
		return E_FAIL;

	return S_OK;
}


bool SpatialAudioRenderer::InitializeSpatialAudio()
{
	auto TraceFail = [](const wchar_t *pStep, HRESULT hr) {
		wchar_t szBuff[256];
		swprintf_s(szBuff, L"[LatmSpatial] InitializeSpatialAudio failed at %s (hr=0x%08X)\n", pStep, hr);
		::OutputDebugStringW(szBuff);
	};

	::OutputDebugStringW(L"[LatmSpatial] InitializeSpatialAudio() called\n");

	if (m_Initialized)
		return true;

	HRESULT hr = ::CoCreateInstance(
		__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
		IID_PPV_ARGS(&m_pDeviceEnumerator));
	if (FAILED(hr)) {
		TraceFail(L"CoCreateInstance(MMDeviceEnumerator)", hr);
		return false;
	}

	hr = m_pDeviceEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &m_pDevice);
	if (FAILED(hr)) {
		TraceFail(L"GetDefaultAudioEndpoint", hr);
		return false;
	}

	hr = m_pDevice->Activate(
		__uuidof(ISpatialAudioClient), CLSCTX_ALL, nullptr, &m_pSpatialAudioClient);
	if (FAILED(hr)) {
		TraceFail(L"IMMDevice::Activate(ISpatialAudioClient)", hr);
		return false;
	}

	UINT32 MaxDynamicObjectCount = 0;
	hr = m_pSpatialAudioClient->GetMaxDynamicObjectCount(&MaxDynamicObjectCount);
	if (FAILED(hr) || (MaxDynamicObjectCount < static_cast<UINT32>(OBJECT_COUNT))) {
		wchar_t szBuff[256];
		swprintf_s(
			szBuff, L"[LatmSpatial] InitializeSpatialAudio failed at GetMaxDynamicObjectCount (hr=0x%08X, count=%u, need=%d)\n",
			hr, MaxDynamicObjectCount, OBJECT_COUNT);
		::OutputDebugStringW(szBuff);
		return false;
	}

	m_hBufferCompletionEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (m_hBufferCompletionEvent == nullptr) {
		TraceFail(L"CreateEvent", ::GetLastError());
		return false;
	}

	const WAVEFORMATEX ObjectFormat = MakeObjectFormat();

	SpatialAudioObjectRenderStreamActivationParams Params{};
	Params.ObjectFormat = &ObjectFormat;
	Params.StaticObjectTypeMask = AudioObjectType_None;
	Params.MinDynamicObjectCount = 0;
	Params.MaxDynamicObjectCount = static_cast<UINT32>(OBJECT_COUNT);
	Params.Category = AudioCategory_Movie;
	Params.EventHandle = m_hBufferCompletionEvent;
	Params.NotifyObject = nullptr;

	PROPVARIANT PropVariant{};
	PropVariant.vt = VT_BLOB;
	PropVariant.blob.cbSize = sizeof(Params);
	PropVariant.blob.pBlobData = reinterpret_cast<BYTE *>(&Params);

	hr = m_pSpatialAudioClient->ActivateSpatialAudioStream(
		&PropVariant, IID_PPV_ARGS(&m_pRenderStream));
	if (FAILED(hr)) {
		TraceFail(L"ActivateSpatialAudioStream", hr);
		return false;
	}

	// 23個の動的オブジェクトを起動時に一括確保し、ストリームが
	// 生きている間ずっと使い回す(22の位置付きch + LFE1/LFE2合成用1個)
	m_Slots.resize(OBJECT_COUNT);

	int SlotIndex = 0;
	for (int i = 0; i < LatmSpatial::RAW_CHANNEL_COUNT; i++) {
		const AVChannel Channel = LatmSpatial::RawChannelOrder[i];
		LatmSpatial::Position3D Position;

		if (!LatmSpatial::GetChannelPosition(Channel, &Position)) {
			// LFE1/LFE2はここに来る(位置情報なし)。専用の1スロットへまとめる
			continue;
		}

		SpatialObjectSlot &Slot = m_Slots[SlotIndex];
		Slot.Position = Position;
		Slot.IsLFE = false;
		Slot.SourceChannelIndices = {i};

		hr = m_pRenderStream->ActivateSpatialAudioObject(
			AudioObjectType_Dynamic, &Slot.Object);
		if (FAILED(hr)) {
			TraceFail(L"ActivateSpatialAudioObject", hr);
			return false;
		}

		SlotIndex++;
	}

	// 最後の1スロットをLFE1+LFE2合成用に割り当てる
	// (RawChannelOrderの末尾2つが LFE1, LFE2 のインデックス)
	{
		SpatialObjectSlot &LFESlot = m_Slots[SlotIndex];
		LFESlot.Position = LatmSpatial::Position3D{0.0f, -0.3f, 0.5f}; // 前方やや下
		LFESlot.IsLFE = true;
		LFESlot.SourceChannelIndices = {
			LatmSpatial::RAW_CHANNEL_COUNT - 2,  // LFE1
			LatmSpatial::RAW_CHANNEL_COUNT - 1}; // LFE2

		hr = m_pRenderStream->ActivateSpatialAudioObject(
			AudioObjectType_Dynamic, &LFESlot.Object);
		if (FAILED(hr)) {
			TraceFail(L"ActivateSpatialAudioObject(LFE)", hr);
			return false;
		}
	}

	// 診断用: 各スロットに設定された最終的な座標をデバッグ出力する。
	// ConvertFrameRaw24ch()側の出力と突き合わせて、「chが見つかって
	// いない」問題なのか「座標がおかしい」問題なのかを切り分けるためのもの。
	// (Release/Debugどちらでも出力される。原因を特定したら削除して良い)
	for (size_t i = 0; i < m_Slots.size(); i++) {
		const SpatialObjectSlot &Slot = m_Slots[i];
		wchar_t szBuff[256];
		swprintf_s(
			szBuff, L"[LatmSpatial] renderer slot %2zu: pos=(%.2f,%.2f,%.2f) IsLFE=%d\n",
			i, Slot.Position.X, Slot.Position.Y, Slot.Position.Z, Slot.IsLFE ? 1 : 0);
		::OutputDebugStringW(szBuff);
	}

	// リングバッファ: 約0.5秒分を確保しておく
	m_RingBufferSamples = SOURCE_SAMPLE_RATE / 2;
	m_RingBuffer.assign(static_cast<size_t>(m_RingBufferSamples) * SOURCE_CHANNELS, 0.0f);
	m_RingWritePos = 0;
	m_RingReadPos = 0;
	m_Buffering = true;
	m_ReadFrac = 0.0;
	m_RateAdjust = 1.0;

	hr = m_pRenderStream->Start();
	if (FAILED(hr)) {
		TraceFail(L"Start", hr);
		return false;
	}

	::OutputDebugStringW(L"[LatmSpatial] InitializeSpatialAudio() succeeded\n");

	m_StopRenderThread = false;
	m_RenderThread = std::thread(&SpatialAudioRenderer::RenderThreadProc, this);

	m_Initialized = true;
	return true;
}


void SpatialAudioRenderer::FinalizeSpatialAudio()
{
	if (!m_Initialized)
		return;

	m_StopRenderThread = true;
	if (m_hBufferCompletionEvent != nullptr)
		::SetEvent(m_hBufferCompletionEvent); // スレッドをWaitから起こす

	if (m_RenderThread.joinable())
		m_RenderThread.join();

	if (m_pRenderStream) {
		m_pRenderStream->Stop();
		m_pRenderStream->Reset();
	}

	for (SpatialObjectSlot &Slot : m_Slots) {
		if (Slot.Object) {
			Slot.Object->SetEndOfStream(0);
			Slot.Object.Reset();
		}
	}
	m_Slots.clear();

	m_pRenderStream.Reset();
	m_pSpatialAudioClient.Reset();
	m_pDevice.Reset();
	m_pDeviceEnumerator.Reset();

	if (m_hBufferCompletionEvent != nullptr) {
		::CloseHandle(m_hBufferCompletionEvent);
		m_hBufferCompletionEvent = nullptr;
	}

	m_Initialized = false;
}


HRESULT SpatialAudioRenderer::StartStreaming()
{
	m_Streaming = true;
	return CBaseRenderer::StartStreaming();
}


HRESULT SpatialAudioRenderer::StopStreaming()
{
	m_Streaming = false;
	return CBaseRenderer::StopStreaming();
}


HRESULT SpatialAudioRenderer::BeginFlush()
{
	::EnterCriticalSection(&m_RingLock);
	m_RingWritePos = 0;
	m_RingReadPos = 0;
	m_Buffering = true;
	m_ReadFrac = 0.0;
	m_RateAdjust = 1.0;
	::LeaveCriticalSection(&m_RingLock);
	return CBaseRenderer::BeginFlush();
}


HRESULT SpatialAudioRenderer::EndFlush()
{
	return CBaseRenderer::EndFlush();
}


STDMETHODIMP SpatialAudioRenderer::NonDelegatingQueryInterface(REFIID riid, void **ppv)
{
	// このレンダラを基準時計として使わせない(詳細はヘッダのコメント参照)。
	// IID_IReferenceClockの問い合わせだけ明示的に失敗させ、それ以外は
	// 通常通りCBaseRendererの実装に任せる
	if (riid == IID_IReferenceClock)
		return E_NOINTERFACE;

	return CBaseRenderer::NonDelegatingQueryInterface(riid, ppv);
}


HRESULT SpatialAudioRenderer::DoRenderSample(IMediaSample *pMediaSample)
{
	if (pMediaSample == nullptr)
		return E_POINTER;

	// 診断用: このメソッドが実際に呼ばれた時刻(壁時計)と、サンプルに
	// 付けられている提示時刻(グラフ時間、100ns単位)を突き合わせる。
	// サンプル自体のタイムスタンプが均等な間隔(約213000=21.3ms)で
	// 増えているのに、壁時計の間隔がそれより大きい(約250ms)場合は、
	// 「タイムスタンプは正常だが、描画のペース待ちが遅すぎる」
	// (基準時計がおかしい)ことになる。タイムスタンプ自体が既に
	// 大きく飛んでいる場合は、上流(AudioDecoderFilter)側の問題になる
	{
		static ULONGLONG LastTick = 0;
		static REFERENCE_TIME LastSampleTime = 0;
		const ULONGLONG NowTick = ::GetTickCount64();
		REFERENCE_TIME Start = 0, End = 0;
		pMediaSample->GetTime(&Start, &End);

		wchar_t szBuff[256];
		swprintf_s(
			szBuff,
			L"[LatmSpatial] DoRenderSample wallclock_delta=%llums sampletime=%lld sampletime_delta=%lld\n",
			(LastTick == 0) ? 0ULL : (NowTick - LastTick),
			static_cast<long long>(Start),
			(LastSampleTime == 0) ? 0LL : static_cast<long long>(Start - LastSampleTime));
		::OutputDebugStringW(szBuff);

		LastTick = NowTick;
		LastSampleTime = Start;
	}

	BYTE *pData = nullptr;
	const HRESULT hr = pMediaSample->GetPointer(&pData);
	if (FAILED(hr) || (pData == nullptr))
		return hr;

	const long DataLength = pMediaSample->GetActualDataLength();
	const int16_t *pSrc = reinterpret_cast<const int16_t *>(pData);
	const size_t SampleCount = static_cast<size_t>(DataLength) / sizeof(int16_t) / SOURCE_CHANNELS;

	::EnterCriticalSection(&m_RingLock);

	for (size_t i = 0; i < SampleCount; i++) {
		const size_t WritePos = m_RingWritePos % m_RingBufferSamples;
		for (int ch = 0; ch < SOURCE_CHANNELS; ch++) {
			const int16_t Sample = pSrc[i * SOURCE_CHANNELS + ch];
			m_RingBuffer[WritePos * SOURCE_CHANNELS + ch] = static_cast<float>(Sample) / 32768.0f;
		}
		m_RingWritePos++;
		// リングが一周してしまった場合、読み取り位置を書き込み位置の
		// 直後まで進める(古いデータを捨てて追従する。無音になるよりはまし)
		if ((m_RingWritePos - m_RingReadPos) > m_RingBufferSamples)
			m_RingReadPos = m_RingWritePos - m_RingBufferSamples;
	}

	::LeaveCriticalSection(&m_RingLock);

	return S_OK;
}


void SpatialAudioRenderer::RenderThreadProc()
{
	// このスレッドをMMCSS(Multimedia Class Scheduler Service)の
	// "Pro Audio" タスクとして登録する。これにより、オーディオ処理用の
	// スレッドとして優先的にスケジューリングされ、他の処理(AACデコード等)
	// でCPUが混み合っていても、バッファ供給が遅れて音が途切れる
	// (無音で埋めてしまう)ことを防ぎやすくなる
	DWORD MmcssTaskIndex = 0;
	const HANDLE hMmcssTask = ::AvSetMmThreadCharacteristics(TEXT("Pro Audio"), &MmcssTaskIndex);
	if (hMmcssTask == nullptr) {
		// MMCSS登録に失敗した場合のフォールバック
		::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	}

	while (!m_StopRenderThread) {
		const DWORD WaitResult = ::WaitForSingleObject(m_hBufferCompletionEvent, 100);
		if (m_StopRenderThread)
			break;
		if (WaitResult != WAIT_OBJECT_0)
			continue; // タイムアウト。念のため次のループへ

		UINT32 AvailableObjectCount = 0;
		UINT32 FrameCount = 0;
		HRESULT hr = m_pRenderStream->BeginUpdatingAudioObjects(&AvailableObjectCount, &FrameCount);
		if (FAILED(hr))
			continue;

		::EnterCriticalSection(&m_RingLock);
		size_t AvailableSamples = m_RingWritePos - m_RingReadPos;

		// --- バッファ量の目安 ---
		// PREFILL/TARGET : 再生開始に必要な量、および通常時に保ちたい量(約100ms)
		// HIGH/TRIM      : 溜まりすぎた場合の上限と、その時に読み飛ばす先
		constexpr double TARGET_SAMPLES = SOURCE_SAMPLE_RATE * 0.100;
		constexpr size_t PREFILL_SAMPLES = static_cast<size_t>(TARGET_SAMPLES);
		constexpr size_t HIGH_WATER_SAMPLES = SOURCE_SAMPLE_RATE * 300 / 1000;
		constexpr size_t TRIM_TARGET_SAMPLES = SOURCE_SAMPLE_RATE * 120 / 1000;
		// 速度補正の最大幅(+-3%。ピッチ変化は約半音の1/2以内)と、
		// 1回(約10ms)ごとに目標値へ近づける割合(急な速度変化を避ける)
		constexpr double MAX_RATE_ADJUST = 0.03;
		constexpr double RATE_SMOOTHING = 0.1;

		if (AvailableSamples > HIGH_WATER_SAMPLES) {
			m_RingReadPos = m_RingWritePos - TRIM_TARGET_SAMPLES;
			AvailableSamples = TRIM_TARGET_SAMPLES;
		}

		// 供給側(デコード/受信)と、この出力デバイスの再生クロックは
		// 完全には一致しない(特に録画/パイプ再生では、供給が実時間より
		// 遅い/速い場合がある)。バッファ量が目標より少なければ少し遅く、
		// 多ければ少し速く読み出すことで、途切れを起こさず追従する
		// (一般的なオーディオレンダラが行うクロック追従の簡易版)
		double Ratio = 1.0;
		size_t OutCount = 0;
		bool EnterBufferingAfter = false;

		if (m_Buffering && (AvailableSamples >= PREFILL_SAMPLES))
			m_Buffering = false;

		if (!m_Buffering) {
			const double AvailD = static_cast<double>(AvailableSamples) - m_ReadFrac;
			double Err = (AvailD - TARGET_SAMPLES) / TARGET_SAMPLES;
			if (Err > 1.0)
				Err = 1.0;
			else if (Err < -1.0)
				Err = -1.0;
			m_RateAdjust += ((1.0 + Err * MAX_RATE_ADJUST) - m_RateAdjust) * RATE_SMOOTHING;
			Ratio = m_RateAdjust;

			// 線形補間のため、読み出し位置の1つ先まで必要。
			// 出力できるサンプル数の上限を求める
			size_t MaxOut = 0;
			if (AvailableSamples >= 2) {
				const double d = (static_cast<double>(AvailableSamples) - 2.0 - m_ReadFrac) / Ratio;
				if (d >= 0.0)
					MaxOut = static_cast<size_t>(d) + 1;
			}
			if (MaxOut >= static_cast<size_t>(FrameCount)) {
				OutCount = static_cast<size_t>(FrameCount);
			} else {
				// 今回で使い切る。残りを短くフェードアウトして出力し、
				// 次回以降は再びプリフィル待ちにする
				OutCount = MaxOut;
				EnterBufferingAfter = true;
			}
		}

		// 各出力サンプルに対応する、リングバッファ上の位置(整数部と小数部)
		if (m_PosIndex.size() < OutCount) {
			m_PosIndex.resize(OutCount);
			m_PosFrac.resize(OutCount);
		}
		for (size_t i = 0; i < OutCount; i++) {
			const double Pos = m_ReadFrac + static_cast<double>(i) * Ratio;
			const size_t Index = static_cast<size_t>(Pos);
			m_PosIndex[i] = Index;
			m_PosFrac[i] = static_cast<float>(Pos - static_cast<double>(Index));
		}

		for (SpatialObjectSlot &Slot : m_Slots) {
			if (!Slot.Object)
				continue;

			Slot.Object->SetPosition(Slot.Position.X, Slot.Position.Y, Slot.Position.Z);

			BYTE *pBuffer = nullptr;
			UINT32 ByteCount = 0;
			if (FAILED(Slot.Object->GetBuffer(&pBuffer, &ByteCount)))
				continue;

			float *pOut = reinterpret_cast<float *>(pBuffer);
			const UINT32 OutSamples = ByteCount / sizeof(float);

			// データ不足(アンダーラン)の場合、無音への切り替わりが
			// 急激だとクリックノイズの原因になるため、境界付近だけ
			// 短くフェードアウトする
			const bool Underrun = OutCount < static_cast<size_t>(OutSamples);
			const UINT32 FadeSamples = Underrun
				? static_cast<UINT32>((std::min)(OutCount, static_cast<size_t>(32)))
				: 0;

			for (UINT32 i = 0; i < OutSamples; i++) {
				if (i >= OutCount) {
					pOut[i] = 0.0f; // データ不足分は無音
					continue;
				}

				const size_t Pos0 = (m_RingReadPos + m_PosIndex[i]) % m_RingBufferSamples;
				const size_t Pos1 = (Pos0 + 1) % m_RingBufferSamples;
				const float Frac = m_PosFrac[i];
				float v = 0.0f;
				for (int SrcCh : Slot.SourceChannelIndices) {
					const float a = m_RingBuffer[Pos0 * SOURCE_CHANNELS + SrcCh];
					const float b = m_RingBuffer[Pos1 * SOURCE_CHANNELS + SrcCh];
					v += a + (b - a) * Frac;
				}

				if (Underrun && (FadeSamples > 0) && ((i + FadeSamples) >= OutCount)) {
					const UINT32 DistanceFromEnd = static_cast<UINT32>(OutCount - i);
					v *= static_cast<float>(DistanceFromEnd) / static_cast<float>(FadeSamples);
				}

				// LFE1+LFE2の単純加算等でフルスケールを超えることがあるため、
				//念のためクリップしておく
				if (v > 1.0f)
					v = 1.0f;
				else if (v < -1.0f)
					v = -1.0f;

				pOut[i] = v;
			}
		}

		if (OutCount > 0) {
			const double Total = m_ReadFrac + static_cast<double>(OutCount) * Ratio;
			const size_t Advance = static_cast<size_t>(Total);
			m_ReadFrac = Total - static_cast<double>(Advance);
			m_RingReadPos += Advance;
		}
		if (EnterBufferingAfter)
			m_Buffering = true;
		::LeaveCriticalSection(&m_RingLock);

		m_pRenderStream->EndUpdatingAudioObjects();
	}

	if (hMmcssTask != nullptr)
		::AvRevertMmThreadCharacteristics(hMmcssTask);
}


}	// namespace LibISDB::DirectShow
