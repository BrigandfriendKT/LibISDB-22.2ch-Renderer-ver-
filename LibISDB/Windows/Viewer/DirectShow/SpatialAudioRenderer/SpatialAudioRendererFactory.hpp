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
 @file   SpatialAudioRendererFactory.hpp
 @brief  SpatialAudioRenderer生成用のファサード
 @author DBCTRADO

 SpatialAudioRenderer.hpp はffmpegのヘッダ(libavutil/channel_layout.h)に
 依存しているため、ViewerFilter.cpp のような、ffmpeg用のインクルード
 パスが設定されていないファイルから直接includeすると、AudioDecoderFilter
 の時と同じビルドエラーになる。このヘッダはffmpegに一切依存しない
 薄いファサードで、実体(SpatialAudioRendererFactory.cpp)側だけが
 SpatialAudioRenderer.hpp をincludeする。
*/


#ifndef LIBISDB_SPATIAL_AUDIO_RENDERER_FACTORY_H
#define LIBISDB_SPATIAL_AUDIO_RENDERER_FACTORY_H


#include <Unknwn.h>


interface IBaseFilter;


namespace LibISDB::DirectShow
{

	/** 現在の環境でSpatialAudioRendererが使用可能かどうかを調べる */
	bool IsSpatialAudioRendererAvailable();

	/**
	 SpatialAudioRendererのインスタンスを作成する。
	 (AudioDecoderFilter::CreateInstance() と同じ流儀。
	  戻り値は AddRef 済みのポインタ)
	*/
	IBaseFilter * WINAPI CreateSpatialAudioRenderer(LPUNKNOWN pUnk, HRESULT *phr);

}	// namespace LibISDB::DirectShow


#endif	// ifndef LIBISDB_SPATIAL_AUDIO_RENDERER_FACTORY_H
