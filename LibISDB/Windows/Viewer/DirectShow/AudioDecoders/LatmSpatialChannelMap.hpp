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
 @file   LatmSpatialChannelMap.hpp
 @brief  22.2ch各チャンネルの3D位置(Windows Spatial Audio向け)
 @author DBCTRADO

 22.2ch(24ch)の各チャンネルを、Windowsの空間オーディオAPI
 (ISpatialAudioObject::SetPosition)で使う3D座標(x, y, z)へ変換するための
 対応表。角度の値は ISO/IEC 23091-3 Table 3 (loudspeaker layout index
 "11/11.2" = 22.2ch構成) に基づく。NHK放送技術研究所の技術資料
 (NHK技研R&D 2018年5月号)にある22.2ch音響の実験設備の仰角(上層+31°/
 下層-29°)とも概ね整合する。

 座標系は +X=右, +Y=上, +Z=正面 を仮定している。ただしWindows APIの
 SetPosition()が実際にどの軸をどちらに取るかは公式ドキュメントに
 明記されていないため、**実機で1チャンネルだけ鳴らして左右上下が
 合っているか耳で確認すること**。ズレていた場合はToXYZ()内のX/Zの
 符号を反転すれば直る見込み。

 LFE1/LFE2(低域効果チャンネル)は指向性を持たせる意味が薄いため、
 この対応表の対象外(GetChannelPosition()はfalseを返す)。
*/


#ifndef LIBISDB_LATM_SPATIAL_CHANNEL_MAP_H
#define LIBISDB_LATM_SPATIAL_CHANNEL_MAP_H


#ifdef LIBISDB_HAS_LIBAV_AAC


extern "C" {
#include <libavutil/channel_layout.h>
}


namespace LibISDB::DirectShow::LatmSpatial
{

	/** Windows空間オーディオAPI向けの3D座標(+X=右, +Y=上, +Z=正面) */
	struct Position3D
	{
		float X;
		float Y;
		float Z;
	};

	/**
	 22.2chの各チャンネルの3D位置を取得する。
	 @param Channel 対象のチャンネル(AVChannel)
	 @param pPosition 結果を格納する
	 @return 位置情報があればtrue。LFE1/LFE2や未対応のチャンネルの場合はfalse
	*/
	bool GetChannelPosition(AVChannel Channel, Position3D *pPosition);

	/** 22.2ch生パススルー時に使う、24ch分の固定出力チャンネル順序 */
	constexpr int RAW_CHANNEL_COUNT = 24;
	extern const AVChannel RawChannelOrder[RAW_CHANNEL_COUNT];

}	// namespace LibISDB::DirectShow::LatmSpatial


#endif	// LIBISDB_HAS_LIBAV_AAC


#endif	// ifndef LIBISDB_LATM_SPATIAL_CHANNEL_MAP_H
