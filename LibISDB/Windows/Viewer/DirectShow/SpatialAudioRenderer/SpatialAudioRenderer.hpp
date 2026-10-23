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
 @file   SpatialAudioRenderer.hpp
 @brief  Windows空間オーディオ(ISpatialAudioClient)を使う、22.2ch用の
         DirectShowレンダラフィルタ
 @author DBCTRADO

 AudioDecoderFilterが「ダウンミックスしない」設定の時に出力する
 24ch・48kHz・16bit PCM(LatmSpatial::RawChannelOrderの順序)を受け取り、
 各チャンネルをWindowsの動的オブジェクト空間オーディオAPI
 (ISpatialAudioObjectRenderStream)へ、LatmSpatialChannelMapの3D位置を
 使って個別に配置して再生する。

 対象はWindows Sonic for Headphonesのみ(v1)。GetMaxDynamicObjectCount()が
 必要数(23個。LFE1/LFE2はまとめて1オブジェクトに合成するため22ch分の
 位置付きch + LFE = 23)に満たない場合、またはそもそも空間オーディオが
 有効になっていない場合は、CheckMediaType()で接続を拒否する
 (グラフ側で別の経路にフォールバックすることを期待する)。

 DirectShowの「サンプルが来たら描画する」タイミングと、Windows空間
 オーディオAPIの「バッファ完了イベント」のタイミングは非同期なため、
 内部を2つに分けている:
   - DoRenderSample(): 受信したPCMをリングバッファ(m_RingBuffer)に
     貯めるだけ
   - RenderThreadProc(): 別スレッドで空間オーディオのバッファ完了
     イベントを待ち、リングバッファから取り出してオブジェクトへ書き込む
*/


#ifndef LIBISDB_SPATIAL_AUDIO_RENDERER_H
#define LIBISDB_SPATIAL_AUDIO_RENDERER_H


#include "../DirectShowBase.hpp"
#include "../AudioDecoders/LatmSpatialChannelMap.hpp"
#include "../../../../Base/DataBuffer.hpp"

#include <SpatialAudioClient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <vector>
#include <atomic>
#include <thread>


namespace LibISDB::DirectShow
{

	/** Windows空間オーディオ(Windows Sonic for Headphones)用レンダラ */
	class SpatialAudioRenderer
		: public CBaseRenderer
	{
	public:
		SpatialAudioRenderer(LPUNKNOWN pUnk, HRESULT *phr);
		~SpatialAudioRenderer();

		// CBaseRenderer
		HRESULT CheckMediaType(const CMediaType *pmt) override;
		HRESULT SetMediaType(const CMediaType *pmt) override;
		HRESULT DoRenderSample(IMediaSample *pMediaSample) override;
		HRESULT StartStreaming() override;
		HRESULT StopStreaming() override;
		HRESULT BeginFlush() override;
		HRESULT EndFlush() override;

		// このレンダラをグラフの基準時計として使わせないためのオーバーライド。
		// (内部でPCMを別スレッド・リングバッファ経由で自前処理しており、
		//  CBaseRendererが標準で提供する基準時計の前提となるタイミング
		//  制御と噛み合わない。基準時計として選ばれると、グラフ全体の
		//  タイミングが大きく狂い、再生が極端にスローになる問題があった)
		STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void **ppv) override;

		/**
		 現在の環境でこのレンダラが使用可能かどうかを調べる。
		 (ISpatialAudioClientの取得・GetMaxDynamicObjectCount()の確認まで
		  行う。実際の接続可否の最終判断はCheckMediaType()で行う)
		*/
		static bool IsAvailable();

		// このレンダラが要求する固定フォーマット(外部からも参照するためpublic)
		static constexpr int SOURCE_CHANNELS = LatmSpatial::RAW_CHANNEL_COUNT; // 24
		static constexpr int SOURCE_SAMPLE_RATE = 48000;
		static constexpr int SOURCE_BITS = 16;

		// LFE1/LFE2はまとめて1個の(非指向性に近い)オブジェクトとして扱うため、
		// 実際に確保する動的オブジェクトの数はRAW_CHANNEL_COUNTより1少ない
		static constexpr int OBJECT_COUNT = SOURCE_CHANNELS - 1; // 23

	private:
		struct SpatialObjectSlot
		{
			Microsoft::WRL::ComPtr<ISpatialAudioObject> Object;
			LatmSpatial::Position3D Position;
			bool IsLFE = false;
			// このオブジェクトへ混ぜる、元の24chのインデックス
			// (LFEスロットのみ2個: LFE1とLFE2)
			std::vector<int> SourceChannelIndices;
		};

		bool InitializeSpatialAudio();
		void FinalizeSpatialAudio();
		void RenderThreadProc();

		Microsoft::WRL::ComPtr<IMMDeviceEnumerator> m_pDeviceEnumerator;
		Microsoft::WRL::ComPtr<IMMDevice> m_pDevice;
		Microsoft::WRL::ComPtr<ISpatialAudioClient> m_pSpatialAudioClient;
		Microsoft::WRL::ComPtr<ISpatialAudioObjectRenderStream> m_pRenderStream;

		std::vector<SpatialObjectSlot> m_Slots; // OBJECT_COUNT個

		HANDLE m_hBufferCompletionEvent;
		std::thread m_RenderThread;
		std::atomic<bool> m_StopRenderThread;
		std::atomic<bool> m_Streaming;

		// DoRenderSample()からRenderThreadProc()へPCMを受け渡すリングバッファ
		// (float、24ch分インターリーブ)
		std::vector<float> m_RingBuffer;
		size_t m_RingBufferSamples; // 1chあたりのサンプル数(容量)
		std::atomic<size_t> m_RingWritePos;
		std::atomic<size_t> m_RingReadPos;
		CRITICAL_SECTION m_RingLock;

		// true の間は、リングバッファに十分な量(プリフィル量)が溜まるまで
		// 無音を出力し続ける(m_RingLockで保護)。アンダーランのたびに
		// 部分的に出力して無音を挟む動作を繰り返すと、約100Hzの音量変動
		// (「ブブブ」という連続したノイズ)になるため、一度不足したら
		// まとまった量が溜まるまで待つ(ヒステリシス)
		bool m_Buffering;

		// 再生速度の微調整(ドリフト補正)用。いずれもm_RingLockで保護
		// m_ReadFrac : リングバッファの読み出し位置の小数部(0以上1未満)
		// m_RateAdjust : 現在の再生速度比(1.0=等速。供給が遅れ気味なら
		//                1未満、溜まりすぎなら1超)
		double m_ReadFrac;
		double m_RateAdjust;

		// RenderThreadProc()内でだけ使う作業用(出力サンプルごとの読み出し位置)
		std::vector<size_t> m_PosIndex;
		std::vector<float> m_PosFrac;

		bool m_Initialized;
	};

}	// namespace LibISDB::DirectShow


#endif	// ifndef LIBISDB_SPATIAL_AUDIO_RENDERER_H
