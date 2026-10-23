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
 @file   LatmDownmixMatrix.hpp
 @brief  AAC-LATM(22.2ch等)ダウンミックス用の、INIファイルで定義可能な
         チャンネルマトリクスエンジン
 @author DBCTRADO

 LatmDownmixPresets.ini (TVTest.exeと同じフォルダに置く。無くても動作する)に、

   [Preset:7.1ch]
   FL=FL:1.0,FLC:0.707107,TFL:0.707107
   FR=FR:1.0,FRC:0.707107,TFR:0.707107
   FC=FC:1.0,TFC:0.707107
   LFE=LFE:1.0,LFE2:1.0
   BL=BL:1.0,TBL:0.707107
   BR=BR:1.0,TBR:0.707107
   SL=SL:1.0,TSL:0.707107
   SR=SR:1.0,TSR:0.707107

 のように [Preset:名前] セクションを複数定義し、各行を
 「出力チャンネル名 = 入力チャンネル名:ゲイン, 入力チャンネル名:ゲイン, ...」
 の形式で記述することで、22.2ch(24ch)入力をどの出力チャンネルへ
 どのゲインで混ぜるかを自由に定義できる。

 ゲインは線形係数(例: 0.707107)、またはdB表記(例: -3dB)のどちらでも指定できる。
 入力チャンネル名を省略した場合(「FL=FL」のように書いた場合)はゲイン1.0とみなす。

 「Gain=」という特別な行(出力チャンネル名ではなく)を書くと、ダウンミックス後
 全体にかける出力ゲイン(ヘッドルーム)を変更できる。省略時は0.9(≒-0.9dB)。
 複数chを足し合わせるため、クリップ防止のマージンとして1.0未満の値を掛けて
 いるが、音量が小さく感じる場合はこの値を大きくする(例: Gain=1.0 で
 ヘッドルームなし。極端な信号でクリップの可能性はあるが、通常の番組音声では
 まず問題にならない)。

 "7.1ch" "5.1ch" という名前のプリセットは組み込みでも用意されており、
 iniファイルが存在しない場合、またはiniに該当セクションが無い場合は
 組み込みの定義が使われる。iniに同名セクションを書けば、その内容で
 完全に上書きされる(組み込み定義とのマージは行わない)。

 現状、下流(AudioDecoderFilter)のパイプラインが2ch/6ch(5.1ch)/8ch(7.1ch)の
 出力しか扱えないため、出力チャンネルは
   2ch: FL,FR
   6ch: FL,FR,FC,LFE,BL,BR
   8ch: FL,FR,FC,LFE,BL,BR,SL,SR
 のいずれかの構成(出力スロットの集合)のみサポートする。
 各スロットへ何をどれだけ混ぜるかは自由に変更できるが、
 22.2chをダウンミックスせず生のまま(24ch)出力することは、
 この仕組みの対象外(別途下流の対応が必要)。

 出力ch数は、プリセット名から以下のように決め打ちで対応付けている
 (iniのセクション名が "2ch"/"5.1ch"/"7.1ch" であることが前提):
   "2ch"   -> 2ch (FL,FR)
   "5.1ch" -> 6ch (FL,FR,FC,LFE,BL,BR)
   "7.1ch" -> 8ch (FL,FR,FC,LFE,BL,BR,SL,SR)
 ユーザーが独自のプリセット名を追加しても、出力ch数は書いてある
 出力チャンネル名の集合から自動判定される(2ch/6ch/8chの
 いずれかとして過不足なく揃っている必要がある)。
*/


#ifndef LIBISDB_LATM_DOWNMIX_MATRIX_H
#define LIBISDB_LATM_DOWNMIX_MATRIX_H


#ifdef LIBISDB_HAS_LIBAV_AAC


#include "../../../../LibISDBBase.hpp"
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
}


namespace LibISDB::DirectShow::LatmDownmix
{

	/** 1つの入力chから1つの出力chへのミックス定義 */
	struct Route
	{
		AVChannel InputChannel;
		float Gain;
	};

	/** 1つの出力chぶんのミックス定義 */
	struct OutputChannelDef
	{
		AVChannel Channel; // この出力スロットが表すスピーカー位置
		std::vector<Route> Routes; // 空の場合はこのchは常に無音
	};

	/**
	 1プリセットぶんのチャンネルマトリクス。
	 OutputChannels の要素数がそのまま出力チャンネル数(6 or 8)になり、
	 並び順がそのままPCM出力時のチャンネル順になる。
	*/
	struct Matrix
	{
		std::vector<OutputChannelDef> OutputChannels;

		// ダウンミックス後、全体にかける出力ゲイン(ヘッドルーム)。
		// 複数chを加算合成するため、クリップを避けるための余裕を持たせる
		// 目的で使う。iniの "Gain=" 行で上書きできる(既定値0.9≒-0.9dB)
		float OutputGain = 0.9f;

		bool IsValid() const noexcept
		{
			return (OutputChannels.size() == 2)
				|| (OutputChannels.size() == 6)
				|| (OutputChannels.size() == 8);
		}
	};

	/** チャンネル名("FL"等)からAVChannelへ変換する。該当なしはAV_CHAN_NONE */
	AVChannel ChannelFromName(const LibISDB::CharType *pName);

	/** AVChannelからチャンネル名へ変換する。該当なしはnullptr */
	const LibISDB::CharType * NameFromChannel(AVChannel Channel);

	/**
	 組み込みのデフォルトプリセットを取得する。
	 @param pName "7.1ch" / "5.1ch" / "2ch"
	 @return 該当プリセットがあればtrue
	*/
	bool GetBuiltinPreset(const LibISDB::CharType *pName, Matrix *pMatrix);

	/**
	 iniファイルから指定した名前のプリセットを読み込む。
	 @param pFileName iniファイルのフルパス
	 @param pPresetName プリセット名(iniでは "[Preset:名前]" のセクションとして定義する)
	 @param pMatrix 読み込んだ結果を格納する
	 @return ファイルが開けて、該当セクションがあり、内容が妥当な場合にtrue。
	         (ファイルが存在しない/セクションが無い/出力ch数が6・8以外、
	          などの場合はfalseを返す。呼び出し側は組み込みプリセットへ
	          フォールバックすること)
	*/
	bool LoadPresetFromFile(
		const LibISDB::CharType *pFileName,
		const LibISDB::CharType *pPresetName,
		Matrix *pMatrix);

	/**
	 iniファイル(あれば)と組み込みプリセットを合わせて、指定した名前の
	 プリセットを解決する。iniに該当セクションがあればそちらを優先し、
	 無ければ組み込み定義(7.1ch/5.1ch)を返す。どちらにも無い場合はfalse。
	*/
	bool ResolvePreset(
		const LibISDB::CharType *pFileName,
		const LibISDB::CharType *pPresetName,
		Matrix *pMatrix);

	/** TVTest.exe と同じフォルダにある LatmDownmixPresets.ini のフルパスを取得する */
	bool GetDefaultPresetFileName(LibISDB::CharType *pFileName, size_t MaxLength);

}	// namespace LibISDB::DirectShow::LatmDownmix


#endif	// LIBISDB_HAS_LIBAV_AAC


#endif	// ifndef LIBISDB_LATM_DOWNMIX_MATRIX_H
