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
 @file   LatmSpatialChannelMap.cpp
 @brief  22.2ch各チャンネルの3D位置(Windows Spatial Audio向け)
 @author DBCTRADO
*/


#include "../../../../LibISDBPrivate.hpp"


#ifdef LIBISDB_HAS_LIBAV_AAC


#include "LatmSpatialChannelMap.hpp"
#include <cmath>


namespace LibISDB::DirectShow::LatmSpatial
{


namespace
{

	// ISO/IEC 23091-3 Table 3 (loudspeaker layout index 11/11.2 = 22.2ch)
	// 方位角: 正面0°、左が正(反時計回り)。仰角: 水平面より上が正
	struct ChannelAngle
	{
		AVChannel Channel;
		float AzimuthDeg;
		float ElevationDeg;
	};

	constexpr ChannelAngle ChannelAngleTable[] = {
		// 中層(仰角0°)
		{AV_CHAN_FRONT_CENTER,             0.0f,   0.0f},
		{AV_CHAN_FRONT_LEFT_OF_CENTER,    30.0f,   0.0f},
		{AV_CHAN_FRONT_RIGHT_OF_CENTER,  -30.0f,   0.0f},
		{AV_CHAN_FRONT_LEFT,              60.0f,   0.0f},
		{AV_CHAN_FRONT_RIGHT,            -60.0f,   0.0f},
		{AV_CHAN_SIDE_LEFT,                90.0f,   0.0f},
		{AV_CHAN_SIDE_RIGHT,              -90.0f,   0.0f},
		{AV_CHAN_BACK_LEFT,               135.0f,   0.0f},
		{AV_CHAN_BACK_RIGHT,             -135.0f,   0.0f},
		{AV_CHAN_BACK_CENTER,             180.0f,   0.0f},
		// 上層(仰角35°、天井中央は90°)
		{AV_CHAN_TOP_FRONT_CENTER,          0.0f,  35.0f},
		{AV_CHAN_TOP_FRONT_LEFT,           45.0f,  35.0f},
		{AV_CHAN_TOP_FRONT_RIGHT,         -45.0f,  35.0f},
		{AV_CHAN_TOP_SIDE_LEFT,            90.0f,  35.0f},
		{AV_CHAN_TOP_SIDE_RIGHT,          -90.0f,  35.0f},
		{AV_CHAN_TOP_CENTER,                0.0f,  90.0f},
		{AV_CHAN_TOP_BACK_LEFT,           135.0f,  35.0f},
		{AV_CHAN_TOP_BACK_RIGHT,         -135.0f,  35.0f},
		{AV_CHAN_TOP_BACK_CENTER,         180.0f,  35.0f},
		// 下層(仰角-15°)
		{AV_CHAN_BOTTOM_FRONT_CENTER,        0.0f, -15.0f},
		{AV_CHAN_BOTTOM_FRONT_LEFT,         45.0f, -15.0f},
		{AV_CHAN_BOTTOM_FRONT_RIGHT,       -45.0f, -15.0f},
		// LFE1/LFE2は指向性を持たないため、この表には含めない
	};

	constexpr float PI = 3.14159265358979323846f;

	Position3D ToXYZ(float AzimuthDeg, float ElevationDeg)
	{
		const float Az = AzimuthDeg * PI / 180.0f;
		const float El = ElevationDeg * PI / 180.0f;

		// +X=右, +Y=上, +Z=正面。
		// 方位角は左が正のため、Xは符号を反転させて計算する
		// (実機で左右が逆に聞こえる場合は、この符号を反転すること)
		return Position3D{
			-std::sin(Az) * std::cos(El),
			 std::sin(El),
			 std::cos(Az) * std::cos(El)
		};
	}

}	// namespace


// 22.2ch生パススルー時の固定チャンネル順序。前半22chはChannelAngleTableと
// 同じ並び(位置付きch)、末尾2chがLFE1/LFE2。この並びは
// AACLATMDecoder_Libavの「ダウンミックスしない」出力と、将来の空間
// オーディオ用レンダラの両方で共通して使う
const AVChannel RawChannelOrder[RAW_CHANNEL_COUNT] = {
	AV_CHAN_FRONT_CENTER,
	AV_CHAN_FRONT_LEFT_OF_CENTER,
	AV_CHAN_FRONT_RIGHT_OF_CENTER,
	AV_CHAN_FRONT_LEFT,
	AV_CHAN_FRONT_RIGHT,
	AV_CHAN_SIDE_LEFT,
	AV_CHAN_SIDE_RIGHT,
	AV_CHAN_BACK_LEFT,
	AV_CHAN_BACK_RIGHT,
	AV_CHAN_BACK_CENTER,
	AV_CHAN_TOP_FRONT_CENTER,
	AV_CHAN_TOP_FRONT_LEFT,
	AV_CHAN_TOP_FRONT_RIGHT,
	AV_CHAN_TOP_SIDE_LEFT,
	AV_CHAN_TOP_SIDE_RIGHT,
	AV_CHAN_TOP_CENTER,
	AV_CHAN_TOP_BACK_LEFT,
	AV_CHAN_TOP_BACK_RIGHT,
	AV_CHAN_TOP_BACK_CENTER,
	AV_CHAN_BOTTOM_FRONT_CENTER,
	AV_CHAN_BOTTOM_FRONT_LEFT,
	AV_CHAN_BOTTOM_FRONT_RIGHT,
	AV_CHAN_LOW_FREQUENCY,
	AV_CHAN_LOW_FREQUENCY_2,
};


bool GetChannelPosition(AVChannel Channel, Position3D *pPosition)
{
	if (pPosition == nullptr)
		return false;

	for (const ChannelAngle &Entry : ChannelAngleTable) {
		if (Entry.Channel == Channel) {
			*pPosition = ToXYZ(Entry.AzimuthDeg, Entry.ElevationDeg);
			return true;
		}
	}

	return false;
}


}	// namespace LibISDB::DirectShow::LatmSpatial


#endif	// LIBISDB_HAS_LIBAV_AAC
