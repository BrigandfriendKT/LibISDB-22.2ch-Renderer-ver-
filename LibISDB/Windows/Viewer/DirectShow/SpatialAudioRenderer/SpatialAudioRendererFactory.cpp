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
 @file   SpatialAudioRendererFactory.cpp
 @brief  SpatialAudioRenderer生成用のファサード
 @author DBCTRADO
*/


#include "../../../../LibISDBPrivate.hpp"
#include "../../../../LibISDBWindows.hpp"
#include "SpatialAudioRendererFactory.hpp"
#include "SpatialAudioRenderer.hpp"
#include "../../../../Base/DebugDef.hpp"


namespace LibISDB::DirectShow
{


bool IsSpatialAudioRendererAvailable()
{
	return SpatialAudioRenderer::IsAvailable();
}


IBaseFilter * WINAPI CreateSpatialAudioRenderer(LPUNKNOWN pUnk, HRESULT *phr)
{
	SpatialAudioRenderer *pNewFilter = new SpatialAudioRenderer(pUnk, phr);
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


}	// namespace LibISDB::DirectShow
