#pragma once

#include "Diff.hpp"
#include "Utils.hpp"
#include <string>
#include <vector>
#include <map>
#include <list>
#include <algorithm>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include "WinWebDiffLib.h"

using WDocument = rapidjson::GenericDocument<rapidjson::UTF16<>>;
using WValue = rapidjson::GenericValue<rapidjson::UTF16<>>;

enum OP_TYPE
{
	OP_NONE = 0, OP_1STONLY, OP_2NDONLY, OP_3RDONLY, OP_DIFF, OP_TRIVIAL
};

struct DiffInfo
{
	DiffInfo(int begin1 = 0, int end1 = 0,
	         int begin2 = 0, int end2 = 0,
	         int begin3 = 0, int end3 = 0)
		: begin{ begin1, begin2, begin3}
		, end{ end1, end2, end3 }
		, nodeIds { 0, 0, 0 }
		, nodePos { 0, 0, 0 }
		, nodeTypes { 0, 0, 0 }
		, op(OP_DIFF)
	{}

	DiffInfo(const DiffInfo& src)
		: nodeIds{ src.nodeIds[0], src.nodeIds[1], src.nodeIds[2] }
		, nodePos{ src.nodePos[0], src.nodePos[1], src.nodePos[2] }
		, nodeTypes{ src.nodeTypes[0], src.nodeTypes[1], src.nodeTypes[2] }
		, begin{ src.begin[0], src.begin[1], src.begin[2] }
		, end{ src.end[0], src.end[1], src.end[2] }
		, op(src.op)
	{}
	int nodeIds[3];
	int nodePos[3];
	int nodeTypes[3];
	int begin[3];
	int end[3];
	OP_TYPE op;
};

struct TextSegment
{
	int nodeId;
	int nodeType;
	size_t begin;
	size_t size;
};

struct TextBlocks
{
	void Make(const WValue& nodeTree)
	{
		const int nodeType = nodeTree[L"nodeType"].GetInt();
		const auto* nodeName = nodeTree[L"nodeName"].GetString();

		if (nodeType == 3 /* TEXT_NODE */)
		{
			std::wstring text = nodeTree[L"nodeValue"].GetString();
			TextSegment seg{};
			seg.nodeId = nodeTree[L"nodeId"].GetInt();
			seg.nodeType = nodeType;
			seg.begin = textBlocks.size();
			seg.size = text.size();
			textBlocks += text;
			segments.insert_or_assign(seg.begin, seg);

		}
		else if (nodeType == 1 /* ELEMENT_NODE */ && 
			wcscmp(nodeName, L"INPUT") == 0)
		{
			const wchar_t* type = getAttribute(nodeTree, L"type");
			if (wcscmp(type, L"hidden") != 0)
			{
				const wchar_t* value = getAttribute(nodeTree, L"value");
				std::wstring text = value ? value : L"";
				TextSegment seg{};
				seg.nodeId = nodeTree[L"nodeId"].GetInt();
				seg.nodeType = nodeType;
				seg.begin = textBlocks.size();
				seg.size = text.size();
				textBlocks += text;
				segments.insert_or_assign(seg.begin, seg);
			}
		}
		if (nodeTree.HasMember(L"children") && nodeTree[L"children"].IsArray())
		{
			if (wcscmp(nodeName, L"SCRIPT") != 0 &&
			    wcscmp(nodeName, L"NOSCRIPT") != 0 &&
			    wcscmp(nodeName, L"NOFRAMES") != 0 &&
			    wcscmp(nodeName, L"STYLE") != 0 &&
			    wcscmp(nodeName, L"TITLE") != 0)
			{
				for (const auto& child : nodeTree[L"children"].GetArray())
				{
					Make(child);
				}
			}
		}
		if (nodeTree.HasMember(L"contentDocument"))
		{
			Make(nodeTree[L"contentDocument"]);
		}
	}

	bool isWordBreak(wchar_t ch)
	{
		static const wchar_t* BreakChars = L".,:;?[](){}<=>`'!\"#$%&^~\\|@+-*/";
		if ((ch & 0xff00) == 0)
		{
			return wcschr(BreakChars, ch) != nullptr;
		}
		else
		{
			WORD wCharType = 0;
			GetStringTypeW(CT_CTYPE1, &ch, 1, &wCharType);
			if ((wCharType & (C1_UPPER | C1_LOWER | C1_DIGIT)) != 0)
				return false;
			return true;
		}
	}
	void Make(const std::wstring& text)
	{
		textBlocks = text;
		int charType = 0;
		int charTypePrev = -1;
		size_t begin = 0;
		for (size_t i = 0; i < text.size(); ++i)
		{
			wchar_t ch = text[i];
			if (iswspace(ch))
				charType = 1;
			else if (isWordBreak(ch))
				charType = 2;
			else
				charType = 0;
			if (charType == 2 || charType != charTypePrev)
			{
				if (i > 0)
				{
					TextSegment seg{};
					seg.nodeId = -1;
					seg.begin = begin;
					seg.size = i - begin;
					segments.insert_or_assign(seg.begin, seg);
					begin = i;
				}
				charTypePrev = charType;
			}
		}
		TextSegment seg{};
		seg.nodeId = -1;
		seg.begin = begin;
		seg.size = text.size() - begin;
		segments.insert_or_assign(seg.begin, seg);
	}

	static const wchar_t* getAttribute(const WValue& node, const wchar_t* name)
	{
		if (!node.HasMember(L"attributes"))
			return nullptr;
		const auto& ary = node[L"attributes"].GetArray();
		for (unsigned i = 0; i < ary.Size(); i += 2)
		{
			if (wcscmp(ary[i].GetString(), name) == 0 && i + 1 < ary.Size())
				return ary[i + 1].GetString();
		}
		return nullptr;
	}

	std::wstring textBlocks;
	std::map<size_t, TextSegment> segments;
};

class DataForDiff
{
public:
	DataForDiff(const TextBlocks& textBlocks, const IWebDiffWindow::DiffOptions& diffOptions) :
		m_textBlocks(textBlocks), m_diffOptions(diffOptions)
	{
	}
	~DataForDiff()
	{
	}
	unsigned size() const { return static_cast<unsigned>(m_textBlocks.textBlocks.size() * sizeof(wchar_t)); }
	const char* data() const { return reinterpret_cast<const char*>(m_textBlocks.textBlocks.data()); }
	const char* next(const char* scanline) const
	{
		auto it = m_textBlocks.segments.find(reinterpret_cast<const wchar_t*>(scanline) - m_textBlocks.textBlocks.data());
		if (it != m_textBlocks.segments.end())
			return scanline + it->second.size * sizeof(wchar_t);
		return nullptr;
	}
	bool match_a_wchar(wchar_t ch1, wchar_t ch2) const
	{
		if (ch1 == ch2)
			return true;
		if (iswupper(ch1))
			ch1 = towlower(ch1);
		if (iswupper(ch2))
			ch2 = towlower(ch2);
		return (ch1 == ch2);
	}
	bool equals(const char* scanline1, unsigned size1,
		const char* scanline2, unsigned size2) const
	{
		if (size1 != size2)
			return false;
		if (!m_diffOptions.ignoreCase && m_diffOptions.ignoreWhitespace == 0)
			return memcmp(scanline1, scanline2, size1) == 0;
		const wchar_t* l1 = reinterpret_cast<const wchar_t*>(scanline1);
		const wchar_t* l2 = reinterpret_cast<const wchar_t*>(scanline2);
		const int s1 = size1 / sizeof(wchar_t);
		const int s2 = size2 / sizeof(wchar_t);
		int i1 = 0, i2 = 0;
		if (m_diffOptions.ignoreWhitespace == 2)
		{
			goto skip_ws;
			while (i1 < s1 && i2 < s2)
			{
				if (!match_a_wchar(l1[i1++], l2[i2++]))
					return false;
			skip_ws:
				while (i1 < s1 && iswspace(l1[i1]))
					i1++;
				while (i2 < s2 && iswspace(l2[i2]))
					i2++;
			}
		}
		else if (m_diffOptions.ignoreWhitespace == 1)
		{
			while (i1 < s1 && i2 < s2)
			{
				if (iswspace(l1[i1]) && iswspace(l2[i2]))
				{
					/* Skip matching spaces and try again */
					while (i1 < s1 && iswspace(l1[i1]))
						i1++;
					while (i2 < s2 && iswspace(l2[i2]))
						i2++;
					continue;
				}
				if (!match_a_wchar(l1[i1++], l2[i2++]))
					return false;
			}
		}
		else
		{
			while (i1 < s1 && i2 < s2)
			{
				if (!match_a_wchar(l1[i1++], l2[i2++]))
					return false;
			}
		}
		return true;
	}
	unsigned long hash_a_wchar(wchar_t ch_) const
	{
		wint_t ch = ch_;
		if (m_diffOptions.ignoreCase && iswupper(ch))
			ch = towlower(ch);
		return ch;
	}
	unsigned long hash(const char* scanline) const
	{
		unsigned long ha = 5381;
		const wchar_t* begin = reinterpret_cast<const wchar_t*>(scanline);
		const wchar_t* end = reinterpret_cast<const wchar_t*>(this->next(scanline));

		if (!m_diffOptions.ignoreCase && m_diffOptions.ignoreWhitespace == 0)
		{
			for (const auto* ptr = begin; ptr < end; ptr++)
			{
				ha += (ha << 5);
				ha ^= *ptr & 0xFF;
			}
			return ha;
		}
		for (const wchar_t* ptr = begin; ptr < end; ptr++)
		{
			if (m_diffOptions.ignoreWhitespace != 0 && iswspace(*ptr))
			{
				while (ptr + 1 < end && iswspace(ptr[1]))
					ptr++;
				if (m_diffOptions.ignoreWhitespace == 2)
					; /* already handled */
				else if (m_diffOptions.ignoreWhitespace == 1)
				{
					ha += (ha << 5);
					ha ^= hash_a_wchar(' ');
				}
				continue;
			}
			ha += (ha << 5);
			ha ^= hash_a_wchar(*ptr);
		}
		return ha;
	}

private:
	const TextBlocks& m_textBlocks;
	const IWebDiffWindow::DiffOptions& m_diffOptions;
};

struct ModifiedNode
{
	int nodeId;
	std::wstring outerHTML;
};

namespace Comparer
{
	template<typename Element, typename Comp02Func>
	std::vector<Element> Make3WayLineDiff(const std::vector<Element>& diff10, const std::vector<Element>& diff12,
		Comp02Func cmpfunc)
	{
		std::vector<Element> diff3;

		size_t diff10count = diff10.size();
		size_t diff12count = diff12.size();

		size_t diff10i = 0;
		size_t diff12i = 0;
		size_t diff3i = 0;

		bool firstDiffBlockIsDiff12;

		Element dr3, dr10, dr12, dr10first, dr10last, dr12first, dr12last;

		int linelast0 = 0;
		int linelast1 = 0;
		int linelast2 = 0;

		for (;;)
		{
			if (diff10i >= diff10count && diff12i >= diff12count)
				break;

			/*
			 * merge overlapped diff blocks
			 * diff10 is diff blocks between file1 and file0.
			 * diff12 is diff blocks between file1 and file2.
			 *
			 *                      diff12
			 *                 diff10            diff3
			 *                 |~~~|             |~~~|
			 * firstDiffBlock  |   |             |   |
			 *                 |   | |~~~|       |   |
			 *                 |___| |   |       |   |
			 *                       |   |   ->  |   |
			 *                 |~~~| |___|       |   |
			 * lastDiffBlock   |   |             |   |
			 *                 |___|             |___|
			 */

			if (diff10i >= diff10count && diff12i < diff12count)
			{
				dr12first = diff12.at(diff12i);
				dr12last = dr12first;
				firstDiffBlockIsDiff12 = true;
			}
			else if (diff10i < diff10count && diff12i >= diff12count)
			{
				dr10first = diff10.at(diff10i);
				dr10last = dr10first;
				firstDiffBlockIsDiff12 = false;
			}
			else
			{
				dr10first = diff10.at(diff10i);
				dr12first = diff12.at(diff12i);
				dr10last = dr10first;
				dr12last = dr12first;
				if (dr12first.begin[0] <= dr10first.begin[0])
					firstDiffBlockIsDiff12 = true;
				else
					firstDiffBlockIsDiff12 = false;
			}
			bool lastDiffBlockIsDiff12 = firstDiffBlockIsDiff12;

			size_t diff10itmp = diff10i;
			size_t diff12itmp = diff12i;
			for (;;)
			{
				if (diff10itmp >= diff10count || diff12itmp >= diff12count)
					break;

				dr10 = diff10.at(diff10itmp);
				dr12 = diff12.at(diff12itmp);

				if (dr10.end[0] == dr12.end[0])
				{
					diff10itmp++;
					lastDiffBlockIsDiff12 = true;

					dr10last = dr10;
					dr12last = dr12;
					break;
				}

				if (lastDiffBlockIsDiff12)
				{
					if ((std::max)(dr12.begin[0], dr12.end[0]) < dr10.begin[0])
						break;
				}
				else
				{
					if ((std::max)(dr10.begin[0], dr10.end[0]) < dr12.begin[0])
						break;
				}

				if (dr12.end[0] > dr10.end[0])
				{
					diff10itmp++;
					lastDiffBlockIsDiff12 = true;
				}
				else
				{
					diff12itmp++;
					lastDiffBlockIsDiff12 = false;
				}

				dr10last = dr10;
				dr12last = dr12;
			}

			if (lastDiffBlockIsDiff12)
				diff12itmp++;
			else
				diff10itmp++;

			if (firstDiffBlockIsDiff12)
			{
				dr3.begin[1] = dr12first.begin[0];
				dr3.begin[2] = dr12first.begin[1];
				if (diff10itmp == diff10i)
					dr3.begin[0] = dr3.begin[1] - linelast1 + linelast0;
				else
					dr3.begin[0] = dr3.begin[1] - dr10first.begin[0] + dr10first.begin[1];
			}
			else
			{
				dr3.begin[0] = dr10first.begin[1];
				dr3.begin[1] = dr10first.begin[0];
				if (diff12itmp == diff12i)
					dr3.begin[2] = dr3.begin[1] - linelast1 + linelast2;
				else
					dr3.begin[2] = dr3.begin[1] - dr12first.begin[0] + dr12first.begin[1];
			}

			if (lastDiffBlockIsDiff12)
			{
				dr3.end[1] = dr12last.end[0];
				dr3.end[2] = dr12last.end[1];
				if (diff10itmp == diff10i)
					dr3.end[0] = dr3.end[1] - linelast1 + linelast0;
				else
					dr3.end[0] = dr3.end[1] - dr10last.end[0] + dr10last.end[1];
			}
			else
			{
				dr3.end[0] = dr10last.end[1];
				dr3.end[1] = dr10last.end[0];
				if (diff12itmp == diff12i)
					dr3.end[2] = dr3.end[1] - linelast1 + linelast2;
				else
					dr3.end[2] = dr3.end[1] - dr12last.end[0] + dr12last.end[1];
			}

			linelast0 = dr3.end[0] + 1;
			linelast1 = dr3.end[1] + 1;
			linelast2 = dr3.end[2] + 1;

			if (diff10i == diff10itmp)
				dr3.op = OP_3RDONLY;
			else if (diff12i == diff12itmp)
				dr3.op = OP_1STONLY;
			else
			{
				if (!cmpfunc(dr3))
					dr3.op = OP_DIFF;
				else
					dr3.op = OP_2NDONLY;
			}

			diff3.push_back(dr3);

			diff3i++;
			diff10i = diff10itmp;
			diff12i = diff12itmp;
		}

		for (size_t i = 0; i < diff3i; i++)
		{
			Element& dr3r = diff3.at(i);
			if (i < diff3i - 1)
			{
				Element& dr3next = diff3.at(i + 1);
				for (int j = 0; j < 3; j++)
				{
					if (dr3r.end[j] >= dr3next.begin[j])
						dr3r.end[j] = dr3next.begin[j] - 1;
				}
			}
		}

		return diff3;
	}

	std::vector<DiffInfo> edscriptToDiffInfo(const std::vector<char>& edscript, const TextBlocks& textBlocks0, const TextBlocks& textBlocks1)
	{
		std::vector<DiffInfo> m_diffInfoList;
		int i0 = 0, i1 = 0;
		auto it0 = textBlocks0.segments.begin();
		auto it1 = textBlocks1.segments.begin();
		for (auto ed : edscript)
		{
			switch (ed)
			{
			case '-':
				m_diffInfoList.emplace_back(i0, i0, i1, i1 - 1);
				++it0;
				++i0;
				break;
			case '+':
				m_diffInfoList.emplace_back(i0, i0 - 1, i1, i1);
				++it1;
				++i1;
				break;
			case '!':
				m_diffInfoList.emplace_back(i0, i0, i1, i1);
				++it0;
				++it1;
				++i0;
				++i1;
				break;
			default:
				++it0;
				++it1;
				++i0;
				++i1;
				break;
			}
		}
		return m_diffInfoList;
	}

	void setNodeIdInDiffInfoList(std::vector<DiffInfo>& m_diffInfoList,
		const std::vector<TextBlocks>& textBlocks)
	{
		for (size_t i = 0; i < m_diffInfoList.size(); ++i)
		{
			for (size_t pane = 0; pane < textBlocks.size(); ++pane)
			{
				auto it = textBlocks[pane].segments.begin();
				std::advance(it, m_diffInfoList[i].begin[pane]);
				if (m_diffInfoList[i].end[pane] < m_diffInfoList[i].begin[pane])
				{
					if (it != textBlocks[pane].segments.begin())
					{
						--it;
						m_diffInfoList[i].nodePos[pane] = 1;
					}
					else
					{
						m_diffInfoList[i].nodePos[pane] = -1;
					}
				}
				else
				{
					m_diffInfoList[i].nodePos[pane] = 0;
				}
				if (it == textBlocks[pane].segments.end())
				{
					m_diffInfoList[i].nodeIds[pane] = -1;
					m_diffInfoList[i].nodeTypes[pane] = -1;
				}
				else
				{
					m_diffInfoList[i].nodeIds[pane] = it->second.nodeId;
					m_diffInfoList[i].nodeTypes[pane] = it->second.nodeType;
				}
			}
		}
	}

	std::vector<DiffInfo> compare(const IWebDiffWindow::DiffOptions& diffOptions,
		std::vector<TextBlocks>& textBlocks)
	{
		DataForDiff data0(textBlocks[0], diffOptions);
		DataForDiff data1(textBlocks[1], diffOptions);
		if (textBlocks.size() < 3)
		{
			Diff<DataForDiff> diff(data0, data1);
			std::vector<char> edscript;

			diff.diff(static_cast<Diff<DataForDiff>::Algorithm>(diffOptions.diffAlgorithm), edscript);
			return edscriptToDiffInfo(edscript, textBlocks[0], textBlocks[1]);
		}

		DataForDiff data2(textBlocks[2], diffOptions);
		Diff<DataForDiff> diff10(data1, data0);
		Diff<DataForDiff> diff12(data1, data2);
		Diff<DataForDiff> diff20(data2, data0);
		std::vector<char> edscript10, edscript12;
		diff10.diff(static_cast<Diff<DataForDiff>::Algorithm>(diffOptions.diffAlgorithm), edscript10);
		diff12.diff(static_cast<Diff<DataForDiff>::Algorithm>(diffOptions.diffAlgorithm), edscript12);
		std::vector<DiffInfo> diffInfoList10 = edscriptToDiffInfo(edscript10, textBlocks[1], textBlocks[0]);
		std::vector<DiffInfo> diffInfoList12 = edscriptToDiffInfo(edscript12, textBlocks[1], textBlocks[2]);

		auto compfunc02 = [&](const DiffInfo & wd3) {
			auto it0 = textBlocks[0].segments.begin();
			auto it2 = textBlocks[2].segments.begin();
			std::advance(it0, wd3.begin[0]);
			if (it0 == textBlocks[0].segments.end())
				return false;
			std::advance(it2, wd3.begin[2]);
			if (it2 == textBlocks[2].segments.end())
				return false;
			unsigned s0 = static_cast<unsigned>(textBlocks[0].segments[it0->second.begin].size) * sizeof(wchar_t);
			unsigned s2 = static_cast<unsigned>(textBlocks[2].segments[it2->second.begin].size) * sizeof(wchar_t);
			return data2.equals(
				reinterpret_cast<const char *>(textBlocks[0].textBlocks.data() + it0->second.begin), s0,
				reinterpret_cast<const char *>(textBlocks[2].textBlocks.data() + it2->second.begin), s2
				);
		};

		return Make3WayLineDiff(diffInfoList10, diffInfoList12, compfunc02);
	}
}

class Highlighter
{
	enum NodeType
	{
		ELEMENT_NODE = 1,
		ATTRIBUTE_NODE = 2,
		TEXT_NODE = 3,
		CDATA_SECTION_NODE = 4,
		PROCESSING_INSTRUCTION_NODE = 7,
		COMMENT_NODE = 8,
		DOCUMENT_NODE = 9,
		DOCUMENT_TYPE_NODE = 10,
		DOCUMENT_FRAGMENT_NODE = 11,
	};

public:
	Highlighter(std::vector<WDocument>& documents,
		std::vector<DiffInfo>& diffInfoList, 
		const IWebDiffWindow::ColorSettings& colorSettings,
		const IWebDiffWindow::DiffOptions& diffOptions,
		bool showWordDifferences,
		int diffIndex)
		: m_documents(documents)
		, m_diffInfoList(diffInfoList)
		, m_colorSettings(colorSettings)
		, m_diffOptions(diffOptions)
		, m_showWordDifferences(showWordDifferences)
		, m_diffIndex(diffIndex)
	{
	}

	void highlightNodes()
	{
		for (size_t i = 0; i < m_diffInfoList.size(); ++i)
		{
			const auto& diffInfo = m_diffInfoList[i];
			WValue* pvalues[3]{};
			std::vector<TextBlocks> textBlocks(m_documents.size());
			std::vector<DiffInfo> wordDiffInfoList;
			for (size_t pane = 0; pane < m_documents.size(); ++pane)
			{
				std::pair<WValue*, WValue*> pair = findNodeId(m_documents[pane][L"root"], diffInfo.nodeIds[pane]);
				pvalues[pane] = pair.first;
				if (diffInfo.nodePos[pane] == 0 && pvalues[pane])
					textBlocks[pane].Make((*pvalues[pane])[L"nodeValue"].GetString());
				else
					textBlocks[pane].Make(L"");
			}
			if (m_showWordDifferences)
				wordDiffInfoList = Comparer::compare(m_diffOptions, textBlocks);
			for (size_t pane = 0; pane < m_documents.size(); ++pane)
			{
				if (!pvalues[pane])
					continue;
				bool snp = false;
				bool deleted = (diffInfo.nodePos[pane] != 0);
				std::wstring className = L"wwd-diff";
				if ((pane == 0 && diffInfo.op == OP_3RDONLY) ||
					(pane == 2 && diffInfo.op == OP_1STONLY))
				{
					snp = true;
					className += deleted ? L" wwd-snpdeleted" : L" wwd-snpchanged";
				}
				else
					className += deleted ? L" wwd-deleted" : L" wwd-changed";
				auto& allocator = m_documents[pane].GetAllocator();
				std::wstring orgtext;
				if (diffInfo.nodePos[pane] == 0)
				{
					if (diffInfo.nodeTypes[pane] == 1 /* ELEMENT_NODE */)
					{
						appendAttributes((*pvalues[pane])[L"attributes"], className, i, allocator);
						(*pvalues[pane]).AddMember(L"modified", true, allocator);
					}
					else if (diffInfo.nodeTypes[pane] == 3 /* TEXT_NODE */)
					{
						WValue spanNode, attributes, children;
						attributes.SetArray();
						appendAttributes(attributes, className, i, textBlocks[pane].textBlocks, allocator);
						spanNode.SetObject();
						spanNode.AddMember(L"nodeName", L"SPAN", allocator);
						spanNode.AddMember(L"attributes", attributes, allocator);
						spanNode.AddMember(L"nodeType", 1, allocator);
						spanNode.AddMember(L"nodeValue", L"", allocator);
						const int nodeId = (*pvalues[pane])[L"nodeId"].GetInt();
						children.SetArray();
						if (m_showWordDifferences && !snp/* && isNeededWordDiffHighlighting(wordDiffInfoList) */)
						{
							makeWordDiffNodes(pane, wordDiffInfoList, textBlocks[pane], children, i == m_diffIndex, allocator);
						}
						else
						{
							WValue textNode;
							textNode.CopyFrom(*pvalues[pane], allocator);
							textNode.RemoveMember(L"modified");
							children.PushBack(textNode, allocator);
						}
						spanNode.AddMember(L"children", children, allocator);
						spanNode.AddMember(L"nodeId", nodeId, allocator);
						spanNode.AddMember(L"modified", true, allocator);
						pvalues[pane]->CopyFrom(spanNode, allocator);
					}
				}
				else
				{
					WValue spanNode, attributes, children;
					attributes.SetArray();
					appendAttributes(attributes, className, i, L"", allocator);
					spanNode.SetObject();
					spanNode.AddMember(L"nodeName", L"SPAN", allocator);
					spanNode.AddMember(L"attributes", attributes, allocator);
					spanNode.AddMember(L"nodeType", 1, allocator);
					spanNode.AddMember(L"nodeValue", L"", allocator);
					spanNode.AddMember(L"nodeId", -1, allocator);
					children.SetArray();
					WValue textNode;
					textNode.SetObject();
					textNode.AddMember(L"nodeId", -1, allocator);
					textNode.AddMember(L"nodeType", 3, allocator);
					textNode.AddMember(L"nodeValue", L"&#8203;", allocator);
					children.PushBack(textNode, allocator);
					spanNode.AddMember(L"children", children, allocator);
					if (diffInfo.nodePos[pane] == -1)
					{
						if (!pvalues[pane]->HasMember(L"insertedNodes"))
						{
							WValue insertedNodes;
							insertedNodes.SetArray();
							pvalues[pane]->AddMember(L"insertedNodes", insertedNodes, allocator);
						}
						(*pvalues[pane])[L"insertedNodes"].GetArray().PushBack(spanNode, allocator);
					}
					else
					{
						if (!pvalues[pane]->HasMember(L"appendedNodes"))
						{
							WValue appendedNodes;
							appendedNodes.SetArray();
							pvalues[pane]->AddMember(L"appendedNodes", appendedNodes, allocator);
						}
						(*pvalues[pane])[L"appendedNodes"].GetArray().PushBack(spanNode, allocator);
					}
					pvalues[pane]->AddMember(L"modified", true, allocator);
				}
			}
		}
	}

	static void unhighlightNodes(WValue& tree, WDocument::AllocatorType& allocator)
	{
		NodeType nodeType = static_cast<NodeType>(tree[L"nodeType"].GetInt());
		switch (nodeType)
		{
		case NodeType::DOCUMENT_NODE:
		{
			if (tree.HasMember(L"children"))
			{
				for (auto& child : tree[L"children"].GetArray())
					unhighlightNodes(child, allocator);
			}
			break;
		}
		case NodeType::ELEMENT_NODE:
		{
			if (isDiffNode(tree))
			{
				const int nodeId = tree[L"nodeId"].GetInt();
				const std::wstring nodeName = tree[L"nodeName"].GetString();
				if (nodeName == L"INPUT")
				{
					removeAttributes(tree[L"attributes"], allocator);
					tree.AddMember(L"modified", true, allocator);
				}
				else
				{
					std::wstring text;
					const wchar_t* value = getAttribute(tree, L"data-wwdtext");
					if (value)
						text = value;
					tree[L"nodeValue"].SetString(text.c_str(), allocator);
					tree[L"nodeType"].SetInt(NodeType::TEXT_NODE);
					tree[L"nodeId"].SetInt(nodeId);
					tree[L"children"].Clear();
					tree.AddMember(L"modified", true, allocator);
				}
			}
			if (tree.HasMember(L"children"))
			{
				for (auto& child : tree[L"children"].GetArray())
					unhighlightNodes(child, allocator);
			}
			if (tree.HasMember(L"contentDocument") && tree[L"contentDocument"].HasMember(L"children"))
			{
				for (auto& child : tree[L"contentDocument"][L"children"].GetArray())
					unhighlightNodes(child, allocator);
			}
			break;
		}
		}
	}

	static std::wstring modifiedNodesToHTMLs(const WValue& tree, std::list<ModifiedNode>& nodes)
	{
		std::wstring html;
		NodeType nodeType = static_cast<NodeType>(tree[L"nodeType"].GetInt());
		switch (nodeType)
		{
		case NodeType::DOCUMENT_TYPE_NODE:
		{
			html += L"<!DOCTYPE ";
			html += tree[L"nodeName"].GetString();
			html += L">";
			break;
		}
		case NodeType::DOCUMENT_NODE:
		{
			if (tree.HasMember(L"children"))
			{
				for (const auto& child : tree[L"children"].GetArray())
					html += modifiedNodesToHTMLs(child, nodes);
			}
			break;
		}
		case NodeType::COMMENT_NODE:
		{
			html += L"<!-- ";
			html += tree[L"nodeValue"].GetString();
			html += L" -->";
			break;
		}
		case NodeType::TEXT_NODE:
		{
			if (tree.HasMember(L"insertedNodes"))
			{
				for (const auto& child : tree[L"insertedNodes"].GetArray())
					html += modifiedNodesToHTMLs(child, nodes);
			}
			std::wstring h = utils::EncodeHTMLEntities(tree[L"nodeValue"].GetString());
			if (!h.empty() && std::all_of(h.begin(), h.end(), [](wchar_t ch) { return ch == ' ' || ch == '\t'; }))
			{
				h.pop_back();
				h += L"&nbsp;";
			}
			html += h;
			if (tree.HasMember(L"appendedNodes"))
			{
				for (const auto& child : tree[L"appendedNodes"].GetArray())
					html += modifiedNodesToHTMLs(child, nodes);
			}
			if (tree.HasMember(L"modified"))
			{
				ModifiedNode node;
				node.nodeId = tree[L"nodeId"].GetInt();
				node.outerHTML = html;
				nodes.emplace_back(std::move(node));
			}
			break;
		}
		case NodeType::ELEMENT_NODE:
		{
			if (tree.HasMember(L"insertedNodes"))
			{
				for (const auto& child : tree[L"insertedNodes"].GetArray())
					html += modifiedNodesToHTMLs(child, nodes);
			}
			html += L'<';
			html += tree[L"nodeName"].GetString();
			if (tree.HasMember(L"attributes"))
			{
				const auto& attributes = tree[L"attributes"].GetArray();
				for (unsigned i = 0; i < attributes.Size(); i += 2)
				{
					html += L" ";
					html += attributes[i].GetString();
					html += L"=\"";
					if (i + 1 < attributes.Size())
						html += utils::EncodeHTMLEntities(attributes[i + 1].GetString());
					html += L"\"";
				}
			}
			html += L'>';
			if (tree.HasMember(L"children"))
			{
				for (const auto& child : tree[L"children"].GetArray())
					html += modifiedNodesToHTMLs(child, nodes);
			}
			if (tree.HasMember(L"appendedNodes"))
			{
				for (const auto& child : tree[L"appendedNodes"].GetArray())
					html += modifiedNodesToHTMLs(child, nodes);
			}
			if (tree.HasMember(L"contentDocument"))
			{
				for (const auto& child : tree[L"contentDocument"][L"children"].GetArray())
					modifiedNodesToHTMLs(child, nodes);
			}
			if (!utils::IsVoidElement(tree[L"nodeName"].GetString()))
			{
				html += L"</";
				html += tree[L"nodeName"].GetString();
				html += L'>';
			}
			if (tree.HasMember(L"modified"))
			{
				ModifiedNode node;
				node.nodeId = tree[L"nodeId"].GetInt();
				node.outerHTML = html;
				nodes.emplace_back(std::move(node));
			}
			break;
		}
		}
		return html;
	}

	static void getDiffNodes(WValue& tree, std::map<int, int>& nodes)
	{
		NodeType nodeType = static_cast<NodeType>(tree[L"nodeType"].GetInt());
		switch (nodeType)
		{
		case NodeType::DOCUMENT_NODE:
		{
			if (tree.HasMember(L"children"))
			{
				for (auto& child : tree[L"children"].GetArray())
					getDiffNodes(child, nodes);
			}
			break;
		}
		case NodeType::ELEMENT_NODE:
		{
			if (isDiffNode(tree))
			{
				const int nodeId = tree[L"nodeId"].GetInt();
				const wchar_t* data = getAttribute(tree, L"data-wwdid");
				const int diffIndex = data ? _wtoi(data) : -1;
				nodes.insert_or_assign(diffIndex, nodeId);
			}
			if (tree.HasMember(L"children"))
			{
				for (auto& child : tree[L"children"].GetArray())
					getDiffNodes(child, nodes);
			}
			if (tree.HasMember(L"contentDocument") && tree[L"contentDocument"].HasMember(L"children"))
			{
				for (auto& child : tree[L"contentDocument"][L"children"].GetArray())
					getDiffNodes(child, nodes);
			}
			break;
		}
		}
	}

	static std::wstring getStyleSheetText(int diffIndex, const IWebDiffWindow::ColorSettings& colorSettings)
	{
		std::wstring styles;
		styles += L" .wwd-changed { " + getDiffStyleValue(colorSettings.clrDiffText, colorSettings.clrDiff) + L" }\n";
		styles += L" .wwd-deleted { " + getDiffStyleValue(colorSettings.clrDiffText, colorSettings.clrDiffDeleted) + L" }\n";
		styles += L" .wwd-snpchanged { " + getDiffStyleValue(colorSettings.clrSNPText, colorSettings.clrSNP) + L" }\n";
		styles += L" .wwd-snpdeleted { " + getDiffStyleValue(colorSettings.clrSNPText, colorSettings.clrSNPDeleted) + L" }\n";
		styles += L" .wwd-word { " + getDiffStyleValue(colorSettings.clrWordDiffText, colorSettings.clrWordDiff) + L" }\n";

		std::wstring datawwdid = L"[data-wwdid=\"" + std::to_wstring(diffIndex) + L"\"]";
		styles += L" .wwd-changed" + datawwdid + L" { " + getDiffStyleValue(colorSettings.clrSelDiffText, colorSettings.clrSelDiff) + L" }\n";
		styles += L" .wwd-deleted" + datawwdid + L" { " + getDiffStyleValue(colorSettings.clrSelDiffText, colorSettings.clrSelDiffDeleted) + L" }\n";
		styles += L" .wwd-snpchanged" + datawwdid + L" { " + getDiffStyleValue(colorSettings.clrSelSNPText, colorSettings.clrSelSNP) + L" }\n";
		styles += L" .wwd-snpdeleted" + datawwdid + L" { " + getDiffStyleValue(colorSettings.clrSelSNPText, colorSettings.clrSelSNPDeleted) + L" }\n";
		styles += L" .wwd-diff" + datawwdid + L" .wwd-word { " + getDiffStyleValue(colorSettings.clrSelWordDiffText, colorSettings.clrSelWordDiff) + L" }\n";
		return styles;
	}

private:
	static void appendAttributes(WValue& attributes, const std::wstring& className, size_t diffIndex, WDocument::AllocatorType& allocator)
	{
		std::wstring className2 = className;
		const auto& ary = attributes.GetArray();
		unsigned i = 0;
		for (i = 0; i < ary.Size(); ++i)
		{
			const auto& el = ary[i];
			if (wcscmp(el.GetString(), L"class") == 0)
			{
				if (i + 1 < ary.Size())
				{
					className2 += L" ";
					className2 += ary[i + 1].GetString();
				}
				break;
			}
		}
		WValue classNameValue(className2.c_str(), static_cast<unsigned>(className2.size()), allocator);
		WValue id(std::to_wstring(diffIndex).c_str(), allocator);
		if (i == ary.Size())
		{
			attributes.PushBack(L"class", allocator);
			attributes.PushBack(classNameValue, allocator);
		}
		else
		{
			if (i + 1 < ary.Size())
				attributes[i + 1].SetString(className2.c_str(), allocator);
			else
				attributes.PushBack(classNameValue, allocator);
		}
		attributes.PushBack(L"data-wwdid", allocator);
		attributes.PushBack(id, allocator);
	}

	static void appendAttributes(WValue& attributes, const std::wstring& className, size_t diffIndex, const std::wstring& orgtext, WDocument::AllocatorType& allocator)
	{
		WValue classNameValue(className.c_str(), static_cast<unsigned>(className.size()), allocator);
		WValue id(std::to_wstring(diffIndex).c_str(), allocator);
		WValue textValue(orgtext.c_str(), static_cast<unsigned>(orgtext.size()), allocator);
		attributes.PushBack(L"class", allocator);
		attributes.PushBack(classNameValue, allocator);
		attributes.PushBack(L"data-wwdid", allocator);
		attributes.PushBack(id, allocator);
		attributes.PushBack(L"data-wwdtext", allocator);
		attributes.PushBack(textValue, allocator);
	}

	static void removeAttributes(WValue& attributes, WDocument::AllocatorType& allocator)
	{
		const auto& ary = attributes.GetArray();
		unsigned i = 0;
		while (i + 1 < ary.Size())
		{
			const auto& el = ary[i];
			if (wcscmp(el.GetString(), L"class") == 0)
			{
				std::wstring classValue = ary[i + 1].GetString();
				size_t pos = 0;
				while ((pos = classValue.find(L"wwd-", pos)) != classValue.npos)
				{
					size_t posend = classValue.find(' ', pos);
					if (posend == classValue.npos)
						classValue.erase(pos);
					else
						classValue.erase(pos, posend + 1 - pos);
				}
				utils::trim_ws(classValue);
				if (classValue.empty())
				{
					ary.Erase(&ary[i]);
					ary.Erase(&ary[i]);
				}
				else
				{
					ary[i + 1].SetString(classValue.c_str(), allocator);
					i += 2;
				}
			}
			else if (wcscmp(el.GetString(), L"data-wwdid") == 0 ||
			         wcscmp(el.GetString(), L"data-wwdtext") == 0)
			{
				ary.Erase(&ary[i]);
				ary.Erase(&ary[i]);
			}
			else
			{
				i += 2;
			}
		}
	}

	static std::wstring getDiffStyleValue(COLORREF color, COLORREF backcolor)
	{
		wchar_t styleValue[256];
		if (color == 0xFFFFFFFF)
			swprintf_s(styleValue, L"background-color: #%02x%02x%02x;",
				GetRValue(backcolor), GetGValue(backcolor), GetBValue(backcolor));
		else
			swprintf_s(styleValue, L"color: #%02x%02x%02x; background-color: #%02x%02x%02x;",
				GetRValue(color), GetGValue(color), GetBValue(color),
				GetRValue(backcolor), GetGValue(backcolor), GetBValue(backcolor));
		return styleValue;
	}

	static std::pair<WValue*, WValue*> findNodeId(WValue& nodeTree, int nodeId)
	{
		if (nodeTree[L"nodeId"].GetInt() == nodeId)
		{
			return { &nodeTree, nullptr };
		}
		if (nodeTree.HasMember(L"children") && nodeTree[L"children"].IsArray())
		{
			for (auto& child : nodeTree[L"children"].GetArray())
			{
				auto [pvalue, pparent] = findNodeId(child, nodeId);
				if (pvalue)
					return { pvalue, pparent ? pparent : &nodeTree };
			}
		}
		if (nodeTree.HasMember(L"contentDocument"))
		{
			auto [pvalue, pparent] = findNodeId(nodeTree[L"contentDocument"], nodeId);
			return { pvalue, pparent ? pparent : &nodeTree[L"contentDocument"] };
		}
		return { nullptr, nullptr };
	}

	static bool containsClassName(const WValue& value, const wchar_t* name)
	{
		if (value[L"nodeType"].GetInt() != NodeType::ELEMENT_NODE)
			return false;
		if (!value.HasMember(L"attributes"))
			return false;
		const auto& ary = value[L"attributes"].GetArray();
		for (unsigned int i = 0; i + 1 < ary.Size(); i += 2)
		{
			if (wcscmp(ary[i].GetString(), L"class") == 0 && 
			    wcsstr(ary[i + 1].GetString(), name) != nullptr)
				return true;
		}
		return false;
	}

	static bool isDiffNode(const WValue& value)
	{
		return containsClassName(value, L"wwd-diff");
	}

	static bool isWordDiffNode(const WValue& value)
	{
		return containsClassName(value, L"wwd-wdiff");
	}

	static const wchar_t* getAttribute(const WValue& node, const wchar_t* name)
	{
		if (!node.HasMember(L"attributes"))
			return nullptr;
		const auto& ary = node[L"attributes"].GetArray();
		for (unsigned i = 0; i < ary.Size(); i += 2)
		{
			if (wcscmp(ary[i].GetString(), name) == 0 && i + 1 < ary.Size())
				return ary[i + 1].GetString();
		}
		return nullptr;
	}

	static void setAttribute(WValue& node, const wchar_t* name, const std::wstring& value, WDocument::AllocatorType& allocator)
	{
		if (!node.HasMember(L"attributes"))
			return;
		const auto& ary = node[L"attributes"].GetArray();
		for (unsigned i = 0; i < ary.Size(); i += 2)
		{
			if (wcscmp(ary[i].GetString(), name) == 0 && i + 1 < ary.Size())
			{
				ary[i + 1].SetString(value.c_str(), static_cast<unsigned>(value.length()), allocator);
				break;
			}
		}
	}

	static void makeTextNode(WValue& textNode, const std::wstring& text, WDocument::AllocatorType& allocator)
	{
		WValue children;
		WValue textValue(text.c_str(), static_cast<unsigned>(text.size()), allocator);
		children.SetArray();
		textNode.SetObject();
		textNode.AddMember(L"nodeId", -1, allocator);
		textNode.AddMember(L"nodeType", 3, allocator);
		textNode.AddMember(L"nodeValue", textValue, allocator);
		textNode.AddMember(L"children", children, allocator);
	}

	bool isNeededWordDiffHighlighting(const std::vector<DiffInfo>& wordDiffInfoList)
	{
		if (wordDiffInfoList.empty())
			return false;
		if (wordDiffInfoList.size() == 1 &&
			wordDiffInfoList[0].end[0] < wordDiffInfoList[0].begin[0] ||
			wordDiffInfoList[0].end[1] < wordDiffInfoList[0].begin[1] ||
			(m_documents.size() > 2 && wordDiffInfoList[0].end[2] < wordDiffInfoList[0].begin[2]))
			return false;
		return true;
	}

	void makeWordDiffNodes(size_t pane, const std::vector<DiffInfo>& wordDiffInfoList,
		const TextBlocks& textBlocks, WValue& children, bool selected, WDocument::AllocatorType& allocator)
	{
		size_t begin = 0;
		for (const auto& diffInfo: wordDiffInfoList)
		{
			auto it = textBlocks.segments.begin();
			std::advance(it, diffInfo.begin[pane]);
			if (it != textBlocks.segments.end())
			{
				size_t begin2 = it->second.begin;
				size_t end2 = 0;
				if (diffInfo.end[pane] != -1)
				{
					it = textBlocks.segments.begin();
					std::advance(it, diffInfo.end[pane]);
					end2 = it->second.begin + it->second.size;
				}
				std::wstring text = textBlocks.textBlocks.substr(begin, begin2 - begin);
				std::wstring textDiff = textBlocks.textBlocks.substr(begin2, end2 - begin2);
				begin = end2;

				if (!text.empty())
				{
					WValue textNode;
					makeTextNode(textNode, text, allocator);
					children.PushBack(textNode, allocator);
				}

				if (!textDiff.empty())
				{
					std::wstring className = L"wwd-wdiff ";
					className += L" wwd-word";
					WValue spanNode, diffTextNode, attributes, spanChildren;
					WValue textDiffValue(textDiff.c_str(), static_cast<unsigned>(textDiff.size()), allocator);
					WValue classNameValue(className.c_str(), static_cast<unsigned>(className.size()), allocator);
					makeTextNode(diffTextNode, textDiff, allocator);
					spanChildren.SetArray();
					spanChildren.PushBack(diffTextNode, allocator);
					attributes.SetArray();
					attributes.PushBack(L"class", allocator);
					attributes.PushBack(classNameValue, allocator);
					spanNode.SetObject();
					spanNode.AddMember(L"nodeId", -1, allocator);
					spanNode.AddMember(L"nodeName", L"SPAN", allocator);
					spanNode.AddMember(L"attributes", attributes, allocator);
					spanNode.AddMember(L"nodeType", 1, allocator);
					spanNode.AddMember(L"nodeValue", L"", allocator);
					spanNode.AddMember(L"children", spanChildren, allocator);
					children.PushBack(spanNode, allocator);
				}
			}
		}
		std::wstring text = textBlocks.textBlocks.substr(begin);
		if (!text.empty())
		{
			WValue textNode;
			makeTextNode(textNode, text, allocator);
			children.PushBack(textNode, allocator);
		}
	}

	std::vector<DiffInfo>& m_diffInfoList;
	std::vector<WDocument>& m_documents;
	const IWebDiffWindow::ColorSettings& m_colorSettings;
	const IWebDiffWindow::DiffOptions& m_diffOptions;
	bool m_showWordDifferences = true;
	int m_diffIndex = -1;
};

