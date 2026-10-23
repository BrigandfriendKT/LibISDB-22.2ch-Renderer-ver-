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
 @file   LatmDownmixMatrix.cpp
 @brief  AAC-LATM(22.2ch等)ダウンミックス用の、INIファイルで定義可能な
         チャンネルマトリクスエンジン
 @author DBCTRADO
*/


#include "../../../../LibISDBPrivate.hpp"


#ifdef LIBISDB_HAS_LIBAV_AAC


#include "../../../../LibISDBWindows.hpp"
#include "LatmDownmixMatrix.hpp"
#include "../../../../Base/DebugDef.hpp"
#include <map>
#include <cwctype>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <iterator>
#include <type_traits>
#include <cwchar>

#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")


namespace LibISDB::DirectShow::LatmDownmix
{


namespace
{

	// チャンネル名 <-> AVChannel 対応表
	// (NHK 22.2ch放送の22スピーカー+LFE2ch、合計24chすべてに対応する)
	struct ChannelNameEntry
	{
		const LibISDB::CharType *Name;
		AVChannel Channel;
	};

	constexpr ChannelNameEntry ChannelNameTable[] = {
		{LIBISDB_STR("FL"),   AV_CHAN_FRONT_LEFT},
		{LIBISDB_STR("FR"),   AV_CHAN_FRONT_RIGHT},
		{LIBISDB_STR("FC"),   AV_CHAN_FRONT_CENTER},
		{LIBISDB_STR("LFE"),  AV_CHAN_LOW_FREQUENCY},
		{LIBISDB_STR("BL"),   AV_CHAN_BACK_LEFT},
		{LIBISDB_STR("BR"),   AV_CHAN_BACK_RIGHT},
		{LIBISDB_STR("FLC"),  AV_CHAN_FRONT_LEFT_OF_CENTER},
		{LIBISDB_STR("FRC"),  AV_CHAN_FRONT_RIGHT_OF_CENTER},
		{LIBISDB_STR("BC"),   AV_CHAN_BACK_CENTER},
		{LIBISDB_STR("SL"),   AV_CHAN_SIDE_LEFT},
		{LIBISDB_STR("SR"),   AV_CHAN_SIDE_RIGHT},
		{LIBISDB_STR("TC"),   AV_CHAN_TOP_CENTER},
		{LIBISDB_STR("TFL"),  AV_CHAN_TOP_FRONT_LEFT},
		{LIBISDB_STR("TFC"),  AV_CHAN_TOP_FRONT_CENTER},
		{LIBISDB_STR("TFR"),  AV_CHAN_TOP_FRONT_RIGHT},
		{LIBISDB_STR("TBL"),  AV_CHAN_TOP_BACK_LEFT},
		{LIBISDB_STR("TBC"),  AV_CHAN_TOP_BACK_CENTER},
		{LIBISDB_STR("TBR"),  AV_CHAN_TOP_BACK_RIGHT},
		{LIBISDB_STR("LFE2"), AV_CHAN_LOW_FREQUENCY_2},
		{LIBISDB_STR("TSL"),  AV_CHAN_TOP_SIDE_LEFT},
		{LIBISDB_STR("TSR"),  AV_CHAN_TOP_SIDE_RIGHT},
		{LIBISDB_STR("BFL"),  AV_CHAN_BOTTOM_FRONT_LEFT},
		{LIBISDB_STR("BFC"),  AV_CHAN_BOTTOM_FRONT_CENTER},
		{LIBISDB_STR("BFR"),  AV_CHAN_BOTTOM_FRONT_RIGHT},
	};

	// 出力スロットとして許可するチャンネルと、その並び順
	// (下流パイプラインが2ch / 6ch=5.1ch / 8ch=7.1chの、この順序の
	//  チャンネル配置しか扱えないための制約)
	constexpr AVChannel Output2ch[] = {
		AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT,
	};
	constexpr AVChannel Output6ch[] = {
		AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT, AV_CHAN_FRONT_CENTER,
		AV_CHAN_LOW_FREQUENCY, AV_CHAN_BACK_LEFT, AV_CHAN_BACK_RIGHT,
	};
	constexpr AVChannel Output8ch[] = {
		AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT, AV_CHAN_FRONT_CENTER,
		AV_CHAN_LOW_FREQUENCY, AV_CHAN_BACK_LEFT, AV_CHAN_BACK_RIGHT,
		AV_CHAN_SIDE_LEFT, AV_CHAN_SIDE_RIGHT,
	};

	// 大文字小文字を無視して比較する
	bool EqualsIgnoreCase(const LibISDB::String &a, const LibISDB::CharType *b)
	{
		size_t i = 0;
		for (; (i < a.size()) && (b[i] != LIBISDB_STR('\0')); i++) {
			if (std::towupper(static_cast<wint_t>(a[i])) != std::towupper(static_cast<wint_t>(b[i])))
				return false;
		}
		return (i == a.size()) && (b[i] == LIBISDB_STR('\0'));
	}

	LibISDB::String Trim(const LibISDB::String &Str)
	{
		size_t Begin = 0, End = Str.size();
		while ((Begin < End) && std::iswspace(static_cast<wint_t>(Str[Begin])))
			Begin++;
		while ((End > Begin) && std::iswspace(static_cast<wint_t>(Str[End - 1])))
			End--;
		return Str.substr(Begin, End - Begin);
	}

	std::vector<LibISDB::String> SplitString(const LibISDB::String &Str, LibISDB::CharType Delimiter)
	{
		std::vector<LibISDB::String> Result;
		size_t Pos = 0;
		for (;;) {
			const size_t Next = Str.find(Delimiter, Pos);
			if (Next == LibISDB::String::npos) {
				Result.push_back(Str.substr(Pos));
				break;
			}
			Result.push_back(Str.substr(Pos, Next - Pos));
			Pos = Next + 1;
		}
		return Result;
	}

	// "-3dB" や "0.707107" のようなゲイン表記を線形係数へ変換する
	bool ParseGain(const LibISDB::String &Str, float *pGain)
	{
		if (Str.empty()) {
			*pGain = 1.0f;
			return true;
		}

		LibISDB::String Value = Str;
		bool IsDecibel = false;

		if (Value.size() >= 2) {
			const LibISDB::CharType c1 = std::towupper(static_cast<wint_t>(Value[Value.size() - 1]));
			const LibISDB::CharType c2 = std::towupper(static_cast<wint_t>(Value[Value.size() - 2]));
			if ((c2 == LIBISDB_STR('D')) && (c1 == LIBISDB_STR('B'))) {
				IsDecibel = true;
				Value = Trim(Value.substr(0, Value.size() - 2));
			}
		}

		LibISDB::CharType *pEnd = nullptr;
		const double Num = std::wcstod(Value.c_str(), &pEnd);
		if ((pEnd == Value.c_str()) || (*pEnd != LIBISDB_STR('\0')))
			return false;

		*pGain = IsDecibel
			? static_cast<float>(std::pow(10.0, Num / 20.0))
			: static_cast<float>(Num);
		return true;
	}

	// テキスト全体から "[Preset:名前]" セクションを抽出する
	// (key, value) のペアを、出現順を保ったまま名前ごとに集める
	using KeyValueList = std::vector<std::pair<LibISDB::String, LibISDB::String>>;

	std::map<LibISDB::String, KeyValueList> ParsePresetSections(const LibISDB::String &Text)
	{
		std::map<LibISDB::String, KeyValueList> Sections;
		std::vector<LibISDB::String> Lines = SplitString(Text, LIBISDB_STR('\n'));

		LibISDB::String CurrentSection;
		bool InPresetSection = false;

		for (LibISDB::String Line : Lines) {
			// 行末の\rや空白を除去し、コメント(;または#以降)を取り除く
			if (!Line.empty() && (Line.back() == LIBISDB_STR('\r')))
				Line.pop_back();
			const size_t CommentPos = Line.find_first_of(LIBISDB_STR(";#"));
			if (CommentPos != LibISDB::String::npos)
				Line = Line.substr(0, CommentPos);
			Line = Trim(Line);
			if (Line.empty())
				continue;

			if ((Line.front() == LIBISDB_STR('[')) && (Line.back() == LIBISDB_STR(']'))) {
				const LibISDB::String Name = Trim(Line.substr(1, Line.size() - 2));
				constexpr LibISDB::CharType Prefix[] = LIBISDB_STR("Preset:");
				constexpr size_t PrefixLength = (sizeof(Prefix) / sizeof(Prefix[0])) - 1;
				if ((Name.size() > PrefixLength) &&
						(Name.compare(0, PrefixLength, Prefix) == 0)) {
					CurrentSection = Name.substr(PrefixLength);
					InPresetSection = true;
					if (Sections.find(CurrentSection) == Sections.end())
						Sections.emplace(CurrentSection, KeyValueList());
				} else {
					InPresetSection = false;
				}
				continue;
			}

			if (!InPresetSection)
				continue;

			const size_t EqPos = Line.find(LIBISDB_STR('='));
			if (EqPos == LibISDB::String::npos)
				continue;

			const LibISDB::String Key = Trim(Line.substr(0, EqPos));
			const LibISDB::String Value = Trim(Line.substr(EqPos + 1));
			if (Key.empty())
				continue;

			Sections[CurrentSection].emplace_back(Key, Value);
		}

		return Sections;
	}

	// (出力ch名, "入力ch:ゲイン,入力ch:ゲイン,...") のリストからMatrixを組み立てる
	bool BuildMatrix(const KeyValueList &KeyValues, Matrix *pMatrix)
	{
		std::map<AVChannel, std::vector<Route>> RoutesByOutput;
		bool HasSide = false;           // SL/SRが定義されている -> 8ch(7.1ch)
		bool HasCenterOrRear = false;    // FC/LFE/BL/BRのいずれかが定義されている -> 6ch(5.1ch)
		float OutputGain = 0.9f; // 既定値。"Gain=" 行があれば上書きされる

		for (const auto &[Key, Value] : KeyValues) {
			if (EqualsIgnoreCase(Key, LIBISDB_STR("Gain"))) {
				if (!ParseGain(Trim(Value), &OutputGain)) {
					LIBISDB_TRACE(
						LIBISDB_STR("LatmDownmixMatrix: Gainの指定が不正です '{}'\n"), Value);
					OutputGain = 0.9f;
				}
				continue;
			}

			const AVChannel OutChannel = ChannelFromName(Key.c_str());
			if (OutChannel == AV_CHAN_NONE) {
				LIBISDB_TRACE(
					LIBISDB_STR("LatmDownmixMatrix: 未知の出力チャンネル名 '{}' を無視します\n"), Key);
				continue;
			}
			if ((OutChannel != AV_CHAN_FRONT_LEFT) && (OutChannel != AV_CHAN_FRONT_RIGHT) &&
					(OutChannel != AV_CHAN_FRONT_CENTER) && (OutChannel != AV_CHAN_LOW_FREQUENCY) &&
					(OutChannel != AV_CHAN_BACK_LEFT) && (OutChannel != AV_CHAN_BACK_RIGHT) &&
					(OutChannel != AV_CHAN_SIDE_LEFT) && (OutChannel != AV_CHAN_SIDE_RIGHT)) {
				// 出力スロットとして使えるのはFL/FR/FC/LFE/BL/BR/SL/SRのみ
				// (下流パイプラインの制約。詳細はヘッダのコメント参照)
				LIBISDB_TRACE(
					LIBISDB_STR("LatmDownmixMatrix: '{}' は出力チャンネルとして使用できません\n"), Key);
				continue;
			}
			if ((OutChannel == AV_CHAN_SIDE_LEFT) || (OutChannel == AV_CHAN_SIDE_RIGHT))
				HasSide = true;
			else if ((OutChannel == AV_CHAN_FRONT_CENTER) || (OutChannel == AV_CHAN_LOW_FREQUENCY) ||
					(OutChannel == AV_CHAN_BACK_LEFT) || (OutChannel == AV_CHAN_BACK_RIGHT))
				HasCenterOrRear = true;

			std::vector<Route> &Routes = RoutesByOutput[OutChannel];
			for (const LibISDB::String &Token : SplitString(Value, LIBISDB_STR(','))) {
				const LibISDB::String T = Trim(Token);
				if (T.empty())
					continue;
				const size_t ColonPos = T.find(LIBISDB_STR(':'));
				const LibISDB::String InName = Trim(T.substr(0, ColonPos));
				const LibISDB::String GainStr =
					(ColonPos == LibISDB::String::npos) ? LibISDB::String() : Trim(T.substr(ColonPos + 1));

				const AVChannel InChannel = ChannelFromName(InName.c_str());
				if (InChannel == AV_CHAN_NONE) {
					LIBISDB_TRACE(
						LIBISDB_STR("LatmDownmixMatrix: 未知の入力チャンネル名 '{}' を無視します\n"), InName);
					continue;
				}
				float Gain;
				if (!ParseGain(GainStr, &Gain)) {
					LIBISDB_TRACE(
						LIBISDB_STR("LatmDownmixMatrix: ゲインの指定が不正です '{}'\n"), GainStr);
					continue;
				}

				Routes.push_back({InChannel, Gain});
			}
		}

		// 出力スロットの集合が 2ch / 5.1ch(6ch) / 7.1ch(8ch) のいずれかとして
		// 過不足なく揃っているかを確認する
		const AVChannel *pRequired =
			HasSide ? Output8ch : HasCenterOrRear ? Output6ch : Output2ch;
		const size_t RequiredCount =
			HasSide ? std::size(Output8ch) : HasCenterOrRear ? std::size(Output6ch) : std::size(Output2ch);

		Matrix Result;
		Result.OutputChannels.reserve(RequiredCount);
		for (size_t i = 0; i < RequiredCount; i++) {
			const auto it = RoutesByOutput.find(pRequired[i]);
			if (it == RoutesByOutput.end()) {
				LIBISDB_TRACE(
					LIBISDB_STR("LatmDownmixMatrix: 出力チャンネル '{}' の定義がありません\n"),
					NameFromChannel(pRequired[i]));
				return false;
			}
			Result.OutputChannels.push_back({pRequired[i], it->second});
		}

		if (!Result.IsValid())
			return false;

		Result.OutputGain = OutputGain;

		*pMatrix = std::move(Result);
		return true;
	}

	// UTF-8(BOM有無どちらも可)のテキストファイルを読み込む
	bool ReadTextFileUtf8(const LibISDB::CharType *pFileName, LibISDB::String *pText)
	{
		std::ifstream File(pFileName, std::ios::binary);
		if (!File.is_open())
			return false;

		std::vector<char> Bytes(
			(std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());

		size_t Offset = 0;
		if ((Bytes.size() >= 3) &&
				(static_cast<unsigned char>(Bytes[0]) == 0xEF) &&
				(static_cast<unsigned char>(Bytes[1]) == 0xBB) &&
				(static_cast<unsigned char>(Bytes[2]) == 0xBF)) {
			Offset = 3;
		}

		if (Bytes.size() <= Offset) {
			pText->clear();
			return true;
		}

		const int Length = ::MultiByteToWideChar(
			CP_UTF8, 0, Bytes.data() + Offset, static_cast<int>(Bytes.size() - Offset), nullptr, 0);
		if (Length <= 0)
			return false;

		std::wstring Wide(static_cast<size_t>(Length), L'\0');
		::MultiByteToWideChar(
			CP_UTF8, 0, Bytes.data() + Offset, static_cast<int>(Bytes.size() - Offset), Wide.data(), Length);

		// このファイルはWindows/DirectShow専用で、LibISDB::CharTypeは
		// 常にwchar_t(LIBISDB_WCHAR定義済み)であることを前提にしている
		static_assert(std::is_same_v<LibISDB::CharType, wchar_t>);
		*pText = Wide;
		return true;
	}

	// 組み込みプリセット定義(ClassifyChannel()相当のゲインをそのまま踏襲)
	// 1/sqrt(2) ≈ -3.0103dB, 0.5 = -6.0206dB という、放送で一般的な
	// 折り込みゲインを使用している
	constexpr LibISDB::CharType BuiltinPresetText[] = LIBISDB_STR(R"INI(
[Preset:7.1ch]
FL=FL:1.0,FLC:0.707107,TFL:0.707107,BFL:0.707107
FR=FR:1.0,FRC:0.707107,TFR:0.707107,BFR:0.707107
FC=FC:1.0,TFC:0.707107,TC:0.5,BFC:0.707107
LFE=LFE:1.0,LFE2:1.0
BL=BL:1.0,BC:0.5,TBL:0.707107,TBC:0.5
BR=BR:1.0,BC:0.5,TBR:0.707107,TBC:0.5
SL=SL:1.0,TSL:0.707107
SR=SR:1.0,TSR:0.707107

[Preset:5.1ch]
FL=FL:1.0,FLC:0.707107,TFL:0.707107,BFL:0.707107
FR=FR:1.0,FRC:0.707107,TFR:0.707107,BFR:0.707107
FC=FC:1.0,TFC:0.707107,TC:0.5,BFC:0.707107
LFE=LFE:1.0,LFE2:1.0
BL=BL:1.0,BC:0.5,TBL:0.707107,TBC:0.5,SL:0.707107,TSL:0.5
BR=BR:1.0,BC:0.5,TBR:0.707107,TBC:0.5,SR:0.707107,TSR:0.5

[Preset:2ch]
Gain=0.4
FL=FL:1.0,FLC:0.707107,TFL:0.5,BFL:0.5,FC:0.707107,TFC:0.5,TC:0.354,BFC:0.5,BL:0.707107,BC:0.354,TBL:0.5,TBC:0.354,SL:0.707107,TSL:0.5,LFE:0.3,LFE2:0.3
FR=FR:1.0,FRC:0.707107,TFR:0.5,BFR:0.5,FC:0.707107,TFC:0.5,TC:0.354,BFC:0.5,BR:0.707107,BC:0.354,TBR:0.5,TBC:0.354,SR:0.707107,TSR:0.5,LFE:0.3,LFE2:0.3
)INI");

}	// namespace


AVChannel ChannelFromName(const LibISDB::CharType *pName)
{
	if (pName == nullptr)
		return AV_CHAN_NONE;
	for (const ChannelNameEntry &Entry : ChannelNameTable) {
		size_t i = 0;
		for (; (Entry.Name[i] != LIBISDB_STR('\0')) && (pName[i] != LIBISDB_STR('\0')); i++) {
			if (std::towupper(static_cast<wint_t>(Entry.Name[i])) != std::towupper(static_cast<wint_t>(pName[i])))
				break;
		}
		if ((Entry.Name[i] == LIBISDB_STR('\0')) && (pName[i] == LIBISDB_STR('\0')))
			return Entry.Channel;
	}
	return AV_CHAN_NONE;
}


const LibISDB::CharType * NameFromChannel(AVChannel Channel)
{
	for (const ChannelNameEntry &Entry : ChannelNameTable) {
		if (Entry.Channel == Channel)
			return Entry.Name;
	}
	return nullptr;
}


bool GetBuiltinPreset(const LibISDB::CharType *pName, Matrix *pMatrix)
{
	if ((pName == nullptr) || (pMatrix == nullptr))
		return false;

	const auto Sections = ParsePresetSections(BuiltinPresetText);
	for (const auto &[Name, KeyValues] : Sections) {
		if (EqualsIgnoreCase(Name, pName))
			return BuildMatrix(KeyValues, pMatrix);
	}
	return false;
}


bool LoadPresetFromFile(
	const LibISDB::CharType *pFileName, const LibISDB::CharType *pPresetName, Matrix *pMatrix)
{
	if ((pFileName == nullptr) || (pPresetName == nullptr) || (pMatrix == nullptr))
		return false;

	LibISDB::String Text;
	if (!ReadTextFileUtf8(pFileName, &Text))
		return false;

	const auto Sections = ParsePresetSections(Text);
	for (const auto &[Name, KeyValues] : Sections) {
		if (EqualsIgnoreCase(Name, pPresetName))
			return BuildMatrix(KeyValues, pMatrix);
	}
	return false;
}


bool ResolvePreset(
	const LibISDB::CharType *pFileName, const LibISDB::CharType *pPresetName, Matrix *pMatrix)
{
	if (LoadPresetFromFile(pFileName, pPresetName, pMatrix)) {
		LIBISDB_TRACE(
			LIBISDB_STR("LatmDownmixMatrix: プリセット '{}' をiniファイルから読み込みました\n"), pPresetName);
		return true;
	}

	if (GetBuiltinPreset(pPresetName, pMatrix)) {
		LIBISDB_TRACE(
			LIBISDB_STR("LatmDownmixMatrix: プリセット '{}' に組み込み定義を使用します\n"), pPresetName);
		return true;
	}

	LIBISDB_TRACE(
		LIBISDB_STR("LatmDownmixMatrix: プリセット '{}' が見つかりません\n"), pPresetName);
	return false;
}


bool GetDefaultPresetFileName(LibISDB::CharType *pFileName, size_t MaxLength)
{
	if ((pFileName == nullptr) || (MaxLength == 0))
		return false;

	TCHAR szPath[MAX_PATH];
	if (::GetModuleFileName(nullptr, szPath, MAX_PATH) == 0)
		return false;
	if (!::PathRemoveFileSpec(szPath))
		return false;
	if (!::PathAppend(szPath, TEXT("LatmDownmixPresets.ini")))
		return false;

#ifdef UNICODE
	if (::wcslen(szPath) >= MaxLength)
		return false;
	::wcscpy_s(pFileName, MaxLength, szPath);
#else
	const int Length = ::MultiByteToWideChar(CP_ACP, 0, szPath, -1, pFileName, static_cast<int>(MaxLength));
	if (Length <= 0)
		return false;
#endif

	return true;
}


}	// namespace LibISDB::DirectShow::LatmDownmix


#endif	// LIBISDB_HAS_LIBAV_AAC
