#pragma once

#include "WinWebDiffLib.h"
#include "WebWindow.hpp"
#include "Utils.hpp"
#include "Diff.hpp"
#include <Windows.h>
#include <shellapi.h>
#include <vector>
#include <map>
#include <wil/win32_helpers.h>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

using WDocument = rapidjson::GenericDocument<rapidjson::UTF16<>>;
using WValue = rapidjson::GenericValue<rapidjson::UTF16<>>;

struct DiffInfo
{
	DiffInfo(int nodeId1 = 0, int nodeId2 = 0, int nodeId3 = 0) :
		nodeIds{ nodeId1, nodeId2, nodeId3 }
	{}

	DiffInfo(const DiffInfo& src) :
		nodeIds{ src.nodeIds[0], src.nodeIds[1], src.nodeIds[2] }
	{}
	int nodeIds[3];
};

struct TextSegment
{
	int nodeId;
	int nodeBegin;
	size_t begin;
	size_t size;
};

struct TextBlocks
{
	void Make(const WValue& nodeTree)
	{
		const int nodeType = nodeTree[L"nodeType"].GetInt();

		if (nodeType == 3 /* TEXT_NODE */)
		{
			std::wstring text = nodeTree[L"nodeValue"].GetString();
			TextSegment seg;
			seg.nodeId = nodeTree[L"nodeId"].GetInt();
			seg.nodeBegin = 0;
			seg.begin = textBlocks.size();
			seg.size = text.size();
			textBlocks += text;
			segments.insert_or_assign(seg.begin, seg);

		}
		if (nodeTree.HasMember(L"children") && nodeTree[L"children"].IsArray())
		{
			const auto* nodeName = nodeTree[L"nodeName"].GetString();
			const bool fInline = utils::IsInlineElement(nodeName);
			if (wcscmp(nodeName, L"SCRIPT") != 0 && wcscmp(nodeName, L"STYLE") != 0)
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
	bool equals(const char* scanline1, unsigned size1,
		const char* scanline2, unsigned size2) const
	{
		if (size1 != size2)
			return false;
		return memcmp(scanline1, scanline2, size1) == 0;
	}
	unsigned long hash(const char* scanline) const
	{
		unsigned long ha = 5381;
		const char* begin = scanline;
		const char* end = this->next(scanline);

		for (const auto* ptr = begin; ptr < end; ptr++)
		{
			ha += (ha << 5);
			ha ^= *ptr & 0xFF;
		}
		return ha;
	}

private:
	const TextBlocks& m_textBlocks;
	const IWebDiffWindow::DiffOptions& m_diffOptions;
};

namespace Comparer
{
	std::vector<DiffInfo> CompareDocuments(const IWebDiffWindow::DiffOptions& diffOptions,
		const std::vector<WDocument>& documents)
	{
		TextBlocks textBlocks0;
		TextBlocks textBlocks1;
		textBlocks0.Make(documents[0][L"root"]);
		textBlocks1.Make(documents[1][L"root"]);
		DataForDiff data1(textBlocks0, diffOptions);
		DataForDiff data2(textBlocks1, diffOptions);
		Diff<DataForDiff> diff(data1, data2);
		std::vector<char> edscript;
		std::vector<DiffInfo> diffInfoList;

		diff.diff(static_cast<Diff<DataForDiff>::Algorithm>(diffOptions.diffAlgorithm), edscript);

		auto it0 = textBlocks0.segments.begin();
		auto it1 = textBlocks1.segments.begin();
		for (auto ed : edscript)
		{
			switch (ed)
			{
			case '-':
				diffInfoList.emplace_back(it0->second.nodeId, -1);
				++it0;
				break;
			case '+':
				diffInfoList.emplace_back(-1, it1->second.nodeId);
				++it1;
				break;
			case '!':
				diffInfoList.emplace_back(it0->second.nodeId, it1->second.nodeId);
				++it0;
				++it1;
				break;
			default:
				++it0;
				++it1;
				break;
			}
		}

		return diffInfoList;
	}
}

class CWebDiffWindow : public IWebDiffWindow
{
public:
	CWebDiffWindow()
	{
	}

	bool Create(HINSTANCE hInstance, HWND hWndParent, int nID, const RECT& rc)
	{
		m_hInstance = hInstance;
		MyRegisterClass(hInstance);
		m_hWnd = CreateWindowExW(0, L"WinWebDiffWindowClass", nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
			rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, hWndParent, reinterpret_cast<HMENU>((intptr_t)nID), hInstance, this);
		return m_hWnd ? true : false;
	}

	bool Destroy()
	{
		BOOL bSucceeded = true;
		if (m_hWnd)
			bSucceeded = DestroyWindow(m_hWnd);
		m_hWnd = nullptr;
		return !!bSucceeded;
	}

	bool IsWebView2Installed() const override
	{
		wil::unique_cotaskmem_string version_info;
		HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version_info);
		return (hr == S_OK && version_info != nullptr);
	}

	bool DownloadWebView2() const override
	{
		return ShellExecute(0, 0, L"https://go.microsoft.com/fwlink/p/?LinkId=2124703", 0, 0, SW_SHOW);
	}

	void AddEventListener(IWebDiffEventHandler* handler) override
	{
		m_listeners.push_back(handler);
	}

	void SetUserDataFolderType(UserdataFolderType userDataFolderType, bool perPane) override
	{
		m_userDataFolderType = userDataFolderType;
		m_bUserDataFolderPerPane = perPane;
	}

	HRESULT New(int nUrls, IWebDiffCallback* callback) override
	{
		const wchar_t* urls[3] = { L"about:blank", L"about:blank", L"about:blank" };
		return Open(nUrls, urls, callback);
	}

	HRESULT Open(const wchar_t* url1, const wchar_t* url2, IWebDiffCallback* callback) override
	{
		const wchar_t* urls[3] = { url1, url2, nullptr };
		return Open(2, urls, callback);
	}

	HRESULT Open(const wchar_t* url1, const wchar_t* url2, const wchar_t* url3, IWebDiffCallback* callback) override
	{
		const wchar_t* urls[3] = { url1, url2, url3 };
		return Open(3, urls, callback);
	}

	HRESULT Open(int nPanes, const wchar_t* urls[3], IWebDiffCallback* callback)
	{
		HRESULT hr = S_OK;
		m_nPanes = nPanes;
		if (m_hWnd)
		{
			Close();
			std::shared_ptr<int> counter(new int{ nPanes });
			for (int i = 0; i < nPanes; ++i)
			{
				std::wstring userDataFolder = GetUserDataFolderPath(i);
				ComPtr<IWebDiffCallback> callback2(callback);
				hr = m_webWindow[i].Create(m_hInstance, m_hWnd, urls[i], userDataFolder.c_str(),
						m_size, m_fitToWindow, m_zoom, m_userAgent, nullptr,
						[this, i, counter, callback2](WebDiffEvent::EVENT_TYPE event)
							{
								WebDiffEvent ev;
								ev.type = event;
								ev.pane = i;
								if (event == WebDiffEvent::SourceChanged)
								{
									m_webWindow[ev.pane].SetZoom(m_zoom);
								}
								else if (event == WebDiffEvent::ZoomFactorChanged)
								{
									m_zoom = m_webWindow[ev.pane].GetZoom();
									for (int pane = 0; pane < m_nPanes; ++pane)
									{
										if (pane != ev.pane)
											m_webWindow[pane].SetZoom(m_zoom);
									}
								}
								else if (event == WebDiffEvent::HSCROLL)
								{
									for (int pane = 0; pane < m_nPanes; ++pane)
									{
										if (pane != ev.pane)
											m_webWindow[pane].SetHScrollPos(m_webWindow[ev.pane].GetHScrollPos());
									}
								}
								else if (event == WebDiffEvent::VSCROLL)
								{
									for (int pane = 0; pane < m_nPanes; ++pane)
									{
										if (pane != ev.pane)
											m_webWindow[pane].SetVScrollPos(m_webWindow[ev.pane].GetVScrollPos());
									}
								}
								else if (event == WebDiffEvent::NavigationCompleted)
								{
									*counter = *counter - 1;
									if (*counter == 0)
										Recompare(callback2.Get());
								}
								for (const auto& listener : m_listeners)
									listener->Invoke(ev);
							});
			}
			std::vector<RECT> rects = CalcChildWebWindowRect(m_hWnd, m_nPanes, m_bHorizontalSplit);
			for (int i = 0; i < m_nPanes; ++i)
				m_webWindow[i].SetWindowRect(rects[i]);
		}
		return hr;
	}

	void Close() override
	{
		for (int i = 0; i < m_nPanes; ++i)
			m_webWindow[i].Destroy();
	}

	void NewTab(int pane, const wchar_t *url, IWebDiffCallback* callback) override
	{
		if (pane < 0 || pane >= m_nPanes || !m_hWnd)
			return;
		m_webWindow[pane].NewTab(url, m_zoom, m_userAgent, callback);
	}

	void CloseActiveTab(int pane) override
	{
		if (pane < 0 || pane >= m_nPanes || !m_hWnd)
			return;
		m_webWindow[pane].CloseActiveTab();
	}

	HRESULT Reload(int pane) override
	{
		if (pane < 0 || pane >= m_nPanes || !m_hWnd)
			return E_INVALIDARG;
		return m_webWindow[pane].Reload();
	}

	HRESULT ReloadAll() override
	{
		HRESULT hr = S_OK;
		for (int pane = 0; pane < m_nPanes; ++pane)
			hr = m_webWindow[pane].Reload();
		return hr;
	}

	HRESULT Recompare(IWebDiffCallback* callback) override
	{
		//static const wchar_t *script = L"document.documentElement.outerHTML";
		static const wchar_t* method = L"DOM.getDocument";
		static const wchar_t* params = L"{ \"depth\": -1, \"pierce\": true }";
		ComPtr<IWebDiffCallback> callback2(callback);
		HRESULT hr = m_webWindow[0].CallDevToolsProtocolMethod(method, params,
			Callback<IWebDiffCallback>([this, callback2](const WebDiffCallbackResult& result) -> HRESULT
				{
					HRESULT hr = result.errorCode;
					if (SUCCEEDED(hr))
					{
						std::wstring json0 = result.returnObjectAsJson;
						hr = m_webWindow[1].CallDevToolsProtocolMethod(method, params,
							Callback<IWebDiffCallback>([this, callback2, json0](const WebDiffCallbackResult& result) -> HRESULT
								{
									HRESULT hr = result.errorCode;
									if (m_nPanes < 3)
									{
										std::vector<WDocument> documents(2);
										documents[0].Parse(json0.c_str());
										documents[1].Parse(result.returnObjectAsJson);
										m_rootNodeId[0] = documents[0][L"root"][L"nodeId"].GetInt();
										m_rootNodeId[1] = documents[1][L"root"][L"nodeId"].GetInt();
										m_diffInfoList = Comparer::CompareDocuments(m_diffOptions, documents);
										HighlightDifferences(m_diffInfoList, documents);
										if (callback2)
											callback2->Invoke(result);
										return S_OK;
									}
									if (SUCCEEDED(hr))
									{
										std::wstring json1 = result.returnObjectAsJson;
										hr = m_webWindow[2].CallDevToolsProtocolMethod(method, params,
											Callback<IWebDiffCallback>([this, callback2, json0, json1](const WebDiffCallbackResult& result) -> HRESULT
												{
													std::vector<WDocument> documents;
													documents[0].Parse(json0.c_str());
													documents[1].Parse(json1.c_str());
													documents[2].Parse(result.returnObjectAsJson);
													m_rootNodeId[0] = documents[0][L"root"][L"nodeId"].GetInt();
													m_rootNodeId[1] = documents[1][L"root"][L"nodeId"].GetInt();
													m_rootNodeId[2] = documents[2][L"root"][L"nodeId"].GetInt();
													m_diffInfoList = Comparer::CompareDocuments(m_diffOptions, documents);
													if (callback2)
														callback2->Invoke(result);
													return S_OK;
												}).Get());
									}
									if (FAILED(hr))
									{
										if (callback2)
											callback2->Invoke({ hr, nullptr });
									}
									return S_OK;
								}).Get());
					}
					if (FAILED(hr))
					{
						if (callback2)
							callback2->Invoke({ hr, nullptr });
					}
					return S_OK;
				}).Get());
		/*
		HRESULT hr = m_webWindow[0].ExecuteScript(script,
			Callback<IWebDiffCallback>([this, callback2](const WebDiffCallbackResult& result) -> HRESULT
				{
					HRESULT hr = result.errorCode;
					if (SUCCEEDED(hr))
					{
						std::wstring json0 = result.returnObjectAsJson;
						hr = m_webWindow[1].ExecuteScript(script,
							Callback<IWebDiffCallback>([this, callback2, json0](const WebDiffCallbackResult& result) -> HRESULT
								{
									HRESULT hr = result.errorCode;
									if (m_nPanes < 3)
									{
										m_diffCount = (json0 == result.returnObjectAsJson) ? 0 : 1;
										if (callback2)
											callback2->Invoke(result);
										return S_OK;
									}
									if (SUCCEEDED(hr))
									{
										std::wstring json1 = result.returnObjectAsJson;
										hr = m_webWindow[2].ExecuteScript(script,
											Callback<IWebDiffCallback>([this, callback2, json0, json1](const WebDiffCallbackResult& result) -> HRESULT
												{
													m_diffCount = (json0 == json1 && json1 == result.returnObjectAsJson) ? 0 : 1;
													if (callback2)
														callback2->Invoke(result);
													return S_OK;
												}).Get());
									}
									if (FAILED(hr))
									{
										if (callback2)
											callback2->Invoke({ hr, nullptr });
									}
									return S_OK;
								}).Get());
					}
					if (FAILED(hr))
					{
						if (callback2)
							callback2->Invoke({ hr, nullptr });
					}
					return S_OK;
				}).Get());
		*/
		return hr;
	}

	HRESULT SaveFile(int pane, FormatType kind, const wchar_t* filename, IWebDiffCallback* callback) override
	{
		if (pane < 0 || pane >= m_nPanes)
			return false;
		return m_webWindow[pane].SaveFile(filename, kind, callback);
	}

	HRESULT SaveFiles(FormatType kind, const wchar_t* filenames[], IWebDiffCallback* callback) override
	{
		std::vector<std::wstring> sfilenames;
		for (int pane = 0; pane < m_nPanes; ++pane)
			sfilenames.push_back(filenames[pane]);
		ComPtr<IWebDiffCallback> callback2(callback);
		HRESULT hr = SaveFile(0, kind, sfilenames[0].c_str(),
			Callback<IWebDiffCallback>([this, kind, sfilenames, callback2](const WebDiffCallbackResult& result) -> HRESULT
				{
					HRESULT hr = result.errorCode;
					if (SUCCEEDED(hr))
					{
						hr = SaveFile(1, kind, sfilenames[1].c_str(),
							Callback<IWebDiffCallback>([this, kind, sfilenames, callback2](const WebDiffCallbackResult& result) -> HRESULT
								{
									HRESULT hr = result.errorCode;
									if (m_nPanes < 3)
									{
										if (callback2)
											callback2->Invoke(result);
										return S_OK;
									}
									if (SUCCEEDED(hr))
									{
										hr = SaveFile(2, kind, sfilenames[2].c_str(),
											Callback<IWebDiffCallback>([this, sfilenames, callback2](const WebDiffCallbackResult& result) -> HRESULT
												{
													if (callback2)
														callback2->Invoke(result);
													return S_OK;
												}).Get());
									}
									if (FAILED(hr))
									{
										if (callback2)
											callback2->Invoke({ hr, nullptr });
									}
									return S_OK;
								}).Get());
					}
					if (FAILED(hr))
					{
						if (callback2)
							callback2->Invoke({ hr, nullptr });
					}
					return S_OK;
				}).Get());
		return hr;
	}

	HRESULT ClearBrowsingData(int pane, BrowsingDataType datakinds) override
	{
		int spane = pane, epane = pane;
		if (pane < 0 || pane >= m_nPanes)
		{
			spane = 0;
			epane = m_nPanes - 1;
		}
		HRESULT hr = S_OK;
		for (int i = spane; i <= epane; ++i)
		{
			if (FAILED(hr = m_webWindow[i].ClearBrowsingData(datakinds)))
				return hr;
		}
		return hr;
	}

	const wchar_t* GetCurrentUrl(int pane) override
	{
		if (pane < 0 || pane >= m_nPanes)
			return L"";
		return m_webWindow[pane].GetCurrentUrl();
	}

	int GetPaneCount() const override
	{
		return m_nPanes;
	}

	RECT GetPaneWindowRect(int pane) const override
	{
		if (pane < 0 || pane >= m_nPanes || !m_hWnd)
		{
			RECT rc = { -1, -1, -1, -1 };
			return rc;
		}
		return m_webWindow[pane].GetWindowRect();
	}

	RECT GetWindowRect() const override
	{
		if (!m_hWnd)
			return RECT{ 0 };
		RECT rc, rcParent;
		HWND hwndParent = GetParent(m_hWnd);
		::GetWindowRect(hwndParent, &rcParent);
		::GetWindowRect(m_hWnd, &rc);
		rc.left -= rcParent.left;
		rc.top -= rcParent.top;
		rc.right -= rcParent.left;
		rc.bottom -= rcParent.top;
		return rc;
	}

	bool SetWindowRect(const RECT& rc) override
	{
		if (m_hWnd)
			MoveWindow(m_hWnd, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
		return true;
	}

	int  GetActivePane() const override
	{
		if (!m_hWnd)
			return -1;
		for (int i = 0; i < m_nPanes; ++i)
			if (m_webWindow[i].IsFocused())
				return i;
		return -1;
	}

	void SetActivePane(int pane) override
	{
		m_webWindow[pane].SetFocus();
	}

	bool GetHorizontalSplit() const override
	{
		return m_bHorizontalSplit;
	}

	void SetHorizontalSplit(bool horizontalSplit) override
	{
		if (!m_hWnd)
			return;
		m_bHorizontalSplit = horizontalSplit;
		std::vector<RECT> rects = CalcChildWebWindowRect(m_hWnd, m_nPanes, m_bHorizontalSplit);
		for (int i = 0; i < m_nPanes; ++i)
			m_webWindow[i].SetWindowRect(rects[i]);
	}

	COLORREF GetDiffColor() const override
	{
		return RGB(0, 0, 0);
	}

	void SetDiffColor(COLORREF clrDiffColor) override
	{
	}

	COLORREF GetSelDiffColor() const override
	{
		return RGB(0, 0, 0);
	}

	void SetSelDiffColor(COLORREF clrSelDiffColor) override
	{
	}

	double GetDiffColorAlpha() const override
	{
		return 0.8;
	}

	void SetDiffColorAlpha(double diffColorAlpha) override
	{
	}

	double GetZoom() const override
	{
		return m_zoom;
	}

	void SetZoom(double zoom) override
	{
		if (zoom == m_zoom)
			return;
		m_zoom = std::clamp(zoom, 0.25, 5.0);
		for (int pane = 0; pane < m_nPanes; ++pane)
			m_webWindow[pane].SetZoom(m_zoom);
	}

	const wchar_t *GetUserAgent() const override
	{
		if (m_nPanes == 0)
			return L"";
		return m_userAgent.c_str();
	}

	void SetUserAgent(const wchar_t* userAgent) override
	{
		m_userAgent = userAgent;
		for (int pane = 0; pane < m_nPanes; ++pane)
			m_webWindow[pane].SetUserAgent(m_userAgent);
	}

	bool GetFitToWindow() const override
	{
		return m_fitToWindow;
	}

	void SetFitToWindow(bool fitToWindow) override
	{
		m_fitToWindow = fitToWindow;
		for (int pane = 0; pane < m_nPanes; ++pane)
			m_webWindow[pane].SetFitToWindow(fitToWindow);
	}

	SIZE GetSize() const override
	{
		return m_size;
	}

	void SetSize(const SIZE size) override
	{
		m_size = size;
		for (int pane = 0; pane < m_nPanes; ++pane)
			m_webWindow[pane].SetSize(size);
	}

	bool GetShowDifferences() const override
	{
		return m_bShowDifferences;
	}

	void SetShowDifferences(bool visible) override
	{
		m_bShowDifferences = visible;
	}

	const DiffOptions& GetDiffOptions() const
	{
		return m_diffOptions;
	}

	void SetDiffOptions(const DiffOptions& diffOptions)
	{
		m_diffOptions = diffOptions;
		Recompare(nullptr);
	}

	int  GetDiffCount() const override
	{
		return static_cast<int>(m_diffInfoList.size());
	}

	int  GetConflictCount() const override
	{
		return 0;
	}

	int  GetCurrentDiffIndex() const override
	{
		return m_currentDiffIndex;
	}

	bool FirstDiff() override
	{
		int oldDiffIndex = m_currentDiffIndex;
		if (m_diffInfoList.size() == 0)
			m_currentDiffIndex = -1;
		else
			m_currentDiffIndex = 0;
		if (oldDiffIndex == m_currentDiffIndex)
			return false;
		selectDiff(m_currentDiffIndex, oldDiffIndex);
		return true;
	}

	bool LastDiff() override
	{
		int oldDiffIndex = m_currentDiffIndex;
		m_currentDiffIndex = static_cast<int>(m_diffInfoList.size()) - 1;
		if (oldDiffIndex == m_currentDiffIndex)
			return false;
		selectDiff(m_currentDiffIndex, oldDiffIndex);
		return true;
	}

	bool NextDiff() override
	{
		int oldDiffIndex = m_currentDiffIndex;
		++m_currentDiffIndex;
		if (m_currentDiffIndex >= m_diffInfoList.size())
			m_currentDiffIndex = static_cast<int>(m_diffInfoList.size()) - 1;
		if (oldDiffIndex == m_currentDiffIndex)
			return false;
		selectDiff(m_currentDiffIndex, oldDiffIndex);
		return true;
	}
	
	bool PrevDiff() override
	{
		int oldDiffIndex = m_currentDiffIndex;
		if (m_diffInfoList.size() == 0)
			m_currentDiffIndex = -1;
		else
		{
			--m_currentDiffIndex;
			if (m_currentDiffIndex < 0)
				m_currentDiffIndex = 0;
		}
		if (oldDiffIndex == m_currentDiffIndex)
			return false;
		selectDiff(m_currentDiffIndex, oldDiffIndex);
		return true;
	}

	bool FirstConflict() override
	{
		return true;
	}

	bool LastConflict() override
	{
		return true;
	}

	bool NextConflict() override
	{
		return true;
	}

	bool PrevConflict()  override
	{
		return true;
	}

	bool SelectDiff(int diffIndex) override
	{
		return selectDiff(diffIndex, m_currentDiffIndex);
	}

	int  GetNextDiffIndex() const override
	{
		if (m_diffInfoList.size() == 0 || m_currentDiffIndex >= m_diffInfoList.size() - 1)
			return -1;
		return m_currentDiffIndex + 1;
	}

	int  GetPrevDiffIndex() const override
	{
		if (m_diffInfoList.size() == 0 || m_currentDiffIndex <= 0)
			return -1;
		return m_currentDiffIndex - 1;
	}

	int  GetNextConflictIndex() const override
	{
		return 0;
	}

	int  GetPrevConflictIndex() const override
	{
		return 0;
	}

	HWND GetHWND() const override
	{
		return m_hWnd;
	}

	HWND GetPaneHWND(int pane) const override
	{
		if (pane < 0 || pane >= m_nPanes)
			return nullptr;
		return m_webWindow[pane].GetHWND();
	}

	bool Copy() override
	{
		return execCommand(L"copy");
	}

	bool Cut() override
	{
		return execCommand(L"cut");
	}

	bool Delete() override
	{
		return execCommand(L"delete");
	}

	bool Paste() override
	{
		return execCommand(L"paste");
	}

	bool SelectAll() override
	{
		return execCommand(L"selectall");
	}

	bool Undo() override
	{
		return execCommand(L"undo");
	}

	bool Redo() override
	{
		return execCommand(L"redo");
	}

	bool CanUndo() override
	{
		return true;
	}

	bool CanRedo() override
	{
		return true;
	}

private:

	const WValue* findNodeId(const WValue& nodeTree, int nodeId)
	{
		if (nodeTree[L"nodeId"].GetInt() == nodeId)
			return &nodeTree;
		if (nodeTree.HasMember(L"children") && nodeTree[L"children"].IsArray())
		{
			for (const auto& child : nodeTree[L"children"].GetArray())
			{
				if (const WValue* pvalue = findNodeId(child, nodeId))
					return pvalue;
			}
		}
		if (nodeTree.HasMember(L"contentDocument"))
			return findNodeId(nodeTree[L"contentDocument"], nodeId);
		return nullptr;
	}

	void HighlightDifferences(std::vector<DiffInfo>& diffInfoList, const std::vector<WDocument>& documents)
	{
		/*
		std::wstring args = L"{ "
			L"\"styleSheetId\": \"wwd\", " 
			L"\"ruleText\": \"\", "
			L"\"location\": {\"startLine: 0, endLine: 0, startColumn: 0, endColumn: 0 } "
			L"}";
		for (int pane = 0; pane < m_nPanes; ++pane)
		{
			m_webWindow[pane].CallDevToolsProtocolMethod(L"CSS.createStyleSheet", args.c_str(),
				Callback<IWebDiffCallback>([this](const WebDiffCallbackResult& result) -> HRESULT
					{
					}).Get());
		}
		*/
		for (size_t i = 0; i < diffInfoList.size(); ++i)
		{
			const auto& diffInfo = diffInfoList[i];
			for (int pane = 0; pane < m_nPanes; ++pane)
			{
				const WValue* pvalue = findNodeId(documents[pane][L"root"], diffInfo.nodeIds[pane]);
				if (pvalue)
				{
					/*
					std::wstring value = L"{ \"nodeId\": "
						+ std::to_wstring(diffInfo.nodeIds[pane])
						+ L", \"value\":\"*"
						+ (*pvalue)[L"nodeValue"].GetString() + L"*\" }";
					m_webWindow[pane].CallDevToolsProtocolMethod(L"DOM.setNodeValue", value.c_str(), nullptr);
					*/
					/*
					std::wstring value = L"{ \"highlightConfig\": { \"contentColor\": \"rgba(111, 168, 220, .66)\" }, \"nodeId\": "
						+ std::to_wstring(diffInfo.nodeIds[pane]) + L" }";
					m_webWindow[pane].CallDevToolsProtocolMethod(L"Overlay.highlightNode", value.c_str(), nullptr);
					/*
					std::wstring outerHTML = std::wstring(L"<span class=\"wwd-diff\" style=\"background-color: #ff0\">")
						+ (*pvalue)[L"nodeValue"].GetString() + L"</span>";
					WDocument args;
					args.SetObject();
					args.AddMember(L"nodeId", diffInfo.nodeIds[pane], args.GetAllocator());
					args.AddMember(L"outerHTML", WValue(outerHTML.c_str(), outerHTML.length()), args.GetAllocator());
					WStringBuffer buffer;
					WPrettyWriter writer(buffer);
					args.Accept(writer);
					m_webWindow[pane].CallDevToolsProtocolMethod(L"DOM.setOuterHTML", buffer.GetString(), nullptr);
					*/
					std::wstring outerHTML = std::wstring(L"<span class=\"wwd-diff\" data-wwdid=\""
						+ std::to_wstring(i)
						+ L"\" style=\"background-color: #ff0\">")
						+ (*pvalue)[L"nodeValue"].GetString() + L"</span>";
					std::wstring args = L"{ \"nodeId\": " + std::to_wstring(diffInfo.nodeIds[pane])
						+ L", \"outerHTML\":" + utils::quote(outerHTML) + L" }";
					m_webWindow[pane].CallDevToolsProtocolMethod(L"DOM.setOuterHTML", args.c_str(), nullptr);
				}
			}
		}

		getDiffNodeIdArray();
	}

	void getDiffNodeIdArray()
	{
		for (int pane = 0; pane < m_nPanes; ++pane)
		{
			std::wstring args = L"{ \"nodeId\": " + std::to_wstring(m_rootNodeId[pane])
				+ L", \"selector\": \"span[data-wwdid]\" }";
			m_webWindow[pane].CallDevToolsProtocolMethod(L"DOM.querySelectorAll", args.c_str(),
				Callback<IWebDiffCallback>([this, pane](const WebDiffCallbackResult& result) -> HRESULT
					{
						WDocument doc;
						doc.Parse(result.returnObjectAsJson);
						auto nodeIds = doc[L"nodeIds"].GetArray();
						for (unsigned i = 0, j = 0; i < nodeIds.Size() && j < m_diffInfoList.size(); ++j)
						{
							if (m_diffInfoList[j].nodeIds[pane] != -1)
								m_diffInfoList[j].nodeIds[pane] = nodeIds[i++].GetInt();
						}
						return S_OK;
					}).Get());
		}
	}

	bool selectDiff(int diffIndex, int prevDiffIndex)
	{
		if (diffIndex < 0 || diffIndex >= m_diffInfoList.size())
			return false;
		for (int pane = 0; pane < m_nPanes; ++pane)
		{
			std::wstring args = L"{ \"nodeId\": " + std::to_wstring(m_diffInfoList[diffIndex].nodeIds[pane]) + L" }";
			m_webWindow[pane].CallDevToolsProtocolMethod(L"DOM.scrollIntoViewIfNeeded", args.c_str(), nullptr);
			m_webWindow[pane].CallDevToolsProtocolMethod(L"DOM.focus", args.c_str(), nullptr);
			if (prevDiffIndex != -1)
			{
				args = L"{ \"nodeId\": " + std::to_wstring(m_diffInfoList[prevDiffIndex].nodeIds[pane])
					+ L", \"name\": \"style\", \"value\": \"background-color: #ff0\" }";
				m_webWindow[pane].CallDevToolsProtocolMethod(L"DOM.setAttributeValue", args.c_str(), nullptr);
			}
			args = L"{ \"nodeId\": " + std::to_wstring(m_diffInfoList[diffIndex].nodeIds[pane])
				+ L", \"name\": \"style\", \"value\": \"background-color: #f00\" }";
			m_webWindow[pane].CallDevToolsProtocolMethod(L"DOM.setAttributeValue", args.c_str(), nullptr);
		}
		return true;
	}

	std::wstring getFromClipboard() const
	{
		std::wstring text;
		if (OpenClipboard(m_hWnd))
		{
			HGLOBAL hData = GetClipboardData(CF_UNICODETEXT);
			if (hData != nullptr)
			{
				LPWSTR pszData = (LPWSTR) GlobalLock(hData);
				if (pszData != nullptr)
				{
					text = pszData;
					GlobalUnlock(hData);
				}
			}
			CloseClipboard();
		}
		return text;
	}

	std::wstring escape(const std::wstring& text) const
	{
		std::wstring result;
		for (auto c : text)
		{
			switch (c)
			{
			case '\r': break;
			case '\n': result += L"\\n"; break;
			case '\"': result += L"\\\""; break;
			case '\\': result += L"\\\\"; break;
			default: result += c;
			}
		}
		return result;
	}

	bool execCommand(const wchar_t *command)
	{
		HWND hwndFocus = GetFocus();
		if (!hwndFocus)
			return false;
		std::wstring cmd = command;
		wchar_t classNameBuf[256]{};
		GetClassName(hwndFocus, classNameBuf, sizeof(classNameBuf) / sizeof(wchar_t));
		std::wstring className = classNameBuf;
		if (className == L"Edit")
		{
			UINT msg = 0;
			WPARAM wParam = 0;
			LPARAM lParam = 0;
			if (cmd == L"copy") { msg = WM_COPY; }
			else if (cmd == L"cut") { msg = WM_CUT; }
			else if (cmd == L"paste") { msg = WM_PASTE; }
			else if (cmd == L"selectall") { msg = EM_SETSEL; wParam = 0; lParam = -1; }
			else if (cmd == L"undo") { msg = WM_UNDO; }
			SendMessage(hwndFocus, msg, wParam, lParam);
			return true;
		}
		int pane = GetActivePane();
		if (pane < 0)
			return false;
		std::wstring script;
		if (cmd != L"paste")
			script = L"document.execCommand(\"" + cmd + L"\")";
		else
		{
			std::wstring text = escape(getFromClipboard());
			script = L"document.execCommand(\"insertText\", false, \"" + text + L"\")";
		}
		return SUCCEEDED(m_webWindow[pane].ExecuteScript(script.c_str(), nullptr));
	}

	std::wstring GetUserDataFolderPath(int pane)
	{
		std::wstring path;
		
		if (m_userDataFolderType == UserdataFolderType::APPDATA)
			path = wil::ExpandEnvironmentStringsW(L"%APPDATA%\\WinMerge\\WinWebDiff\\").get();
		else
			path = wil::GetModuleFileNameW(GetModuleHandle(nullptr)).get() + std::wstring(L".WebView2");
		if (m_bUserDataFolderPerPane)
			path += L"\\" + std::to_wstring(pane + 1);
		return path;
	}

	std::vector<RECT> CalcChildWebWindowRect(HWND hWnd, int nPanes, bool bHorizontalSplit)
	{
		std::vector<RECT> childrects;
		RECT rcParent;
		GetClientRect(hWnd, &rcParent);
		RECT rc = rcParent;
		if (nPanes > 0)
		{
			if (!bHorizontalSplit)
			{
				int cx = 0; //GetSystemMetrics(SM_CXVSCROLL);
				int width = (rcParent.left + rcParent.right - cx) / nPanes - 2;
				rc.left = 0;
				rc.right = rc.left + width;
				for (int i = 0; i < nPanes - 1; ++i)
				{
					childrects.push_back(rc);
					rc.left = rc.right + 2 * 2;
					rc.right = rc.left + width;
				}
				rc.right = rcParent.right;
				rc.left = rc.right - width - cx;
				childrects.push_back(rc);
			}
			else
			{
				int cy = 0; // GetSystemMetrics(SM_CXVSCROLL);
				int height = (rcParent.top + rcParent.bottom - cy) / nPanes - 2;
				rc.top = 0;
				rc.bottom = rc.top + height;
				for (int i = 0; i < nPanes - 1; ++i)
				{
					childrects.push_back(rc);
					rc.top = rc.bottom + 2 * 2;
					rc.bottom = rc.top + height;
				}
				rc.bottom = rcParent.bottom;
				rc.top = rc.bottom - height - cy;
				childrects.push_back(rc);
			}
		}
		return childrects;
	}

	void MoveSplitter(int x, int y)
	{
		RECT rcParent;
		GetClientRect(m_hWnd, &rcParent);

		RECT rc[3];
		for (int i = 0; i < m_nPanes; ++i)
			rc[i] = m_webWindow[i].GetWindowRect();

		if (!m_bHorizontalSplit)
		{
			int minx = rc[m_nDraggingSplitter].left + 32;
			int maxx = rc[m_nDraggingSplitter + 1].right - 32;
			if (x < minx)
				rc[m_nDraggingSplitter].right = minx;
			else if (x > maxx)
				rc[m_nDraggingSplitter].right = maxx;
			else
				rc[m_nDraggingSplitter].right = x;
			for (int i = m_nDraggingSplitter + 1; i < m_nPanes; ++i)
			{
				int width = rc[i].right - rc[i].left;
				rc[i].left = rc[i - 1].right + 2 * 2;
				rc[i].right = rc[i].left + width;
			}
			rc[m_nPanes - 1].right = rcParent.right;
		}
		else
		{
			rc[m_nDraggingSplitter].bottom = y;
			int miny = rc[m_nDraggingSplitter].top + 32;
			int maxy = rc[m_nDraggingSplitter + 1].bottom - 32;
			if (y < miny)
				rc[m_nDraggingSplitter].bottom = miny;
			else if (y > maxy)
				rc[m_nDraggingSplitter].bottom = maxy;
			else
				rc[m_nDraggingSplitter].bottom = y;
			for (int i = m_nDraggingSplitter + 1; i < m_nPanes; ++i)
			{
				int height = rc[i].bottom - rc[i].top;
				rc[i].top = rc[i - 1].bottom + 2 * 2;
				rc[i].bottom = rc[i].top + height;
			}
			rc[m_nPanes - 1].bottom = rcParent.bottom;
		}

		for (int i = 0; i < m_nPanes; ++i)
			m_webWindow[i].SetWindowRect(rc[i]);
	}

	void OnCreate(HWND hWnd, LPCREATESTRUCT lpCreateStruct)
	{
	}

	void OnSize(UINT nType, int cx, int cy)
	{
		std::vector<RECT> rects = CalcChildWebWindowRect(m_hWnd, m_nPanes, m_bHorizontalSplit);
		for (int i = 0; i < m_nPanes; ++i)
			m_webWindow[i].SetWindowRect(rects[i]);
	}

	void OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags)
	{
	}

	void OnLButtonDown(UINT nFlags, int x, int y)
	{
		int i;
		for (i = 0; i < m_nPanes - 1; ++i)
		{
			if (!m_bHorizontalSplit)
			{
				if (x < m_webWindow[i + 1].GetWindowRect().left)
					break;
			}
			else
			{
				if (y < m_webWindow[i + 1].GetWindowRect().top)
					break;
			}
		}
		m_nDraggingSplitter = i;
		SetCapture(m_hWnd);
	}

	void OnLButtonUp(UINT nFlags, int x, int y)
	{
		if (m_nDraggingSplitter == -1)
			return;
		ReleaseCapture();
		MoveSplitter(x, y);
		m_nDraggingSplitter = -1;
	}

	void OnMouseMove(UINT nFlags, int x, int y)
	{
		if (m_nPanes < 2)
			return;
		if (m_nDraggingSplitter == -1)
			return;
		MoveSplitter(x, y);
	}

	LRESULT OnWndMsg(HWND hwnd, UINT iMsg, WPARAM wParam, LPARAM lParam)
	{
		switch (iMsg)
		{
		case WM_CREATE:
			OnCreate(hwnd, (LPCREATESTRUCT)lParam);
			break;
		case WM_COMMAND:
			PostMessage(GetParent(m_hWnd), iMsg, wParam, lParam);
			break;
		case WM_SIZE:
			OnSize((UINT)wParam, LOWORD(lParam), HIWORD(lParam));
			break;
		case WM_KEYDOWN:
			OnKeyDown((UINT)wParam, (int)(short)LOWORD(lParam), (UINT)HIWORD(lParam));
			break;
		case WM_LBUTTONDOWN:
			OnLButtonDown((UINT)(wParam), (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam));
			break;
		case WM_LBUTTONUP:
			OnLButtonUp((UINT)(wParam), (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam));
			break;
		case WM_MOUSEMOVE:
			OnMouseMove((UINT)(wParam), (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam));
			break;
		case WM_SETCURSOR:
			if ((HWND)wParam == m_hWnd)
			{
				SetCursor(::LoadCursor(nullptr, !m_bHorizontalSplit ? IDC_SIZEWE : IDC_SIZENS));
				return TRUE;
			}
			break;
		default:
			return DefWindowProc(hwnd, iMsg, wParam, lParam);
		}
		return 0;
	}

	static LRESULT CALLBACK WndProc(HWND hwnd, UINT iMsg, WPARAM wParam, LPARAM lParam)
	{
		if (iMsg == WM_NCCREATE)
			SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<LPCREATESTRUCT>(lParam)->lpCreateParams));
		CWebDiffWindow* pWebWnd = reinterpret_cast<CWebDiffWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
		LRESULT lResult = pWebWnd->OnWndMsg(hwnd, iMsg, wParam, lParam);
		return lResult;
	}

	ATOM MyRegisterClass(HINSTANCE hInstance)
	{
		WNDCLASSEXW wcex = { 0 };
		wcex.cbSize = sizeof(WNDCLASSEX);
		wcex.style = CS_HREDRAW | CS_VREDRAW;
		wcex.lpfnWndProc = (WNDPROC)CWebDiffWindow::WndProc;
		wcex.cbClsExtra = 0;
		wcex.cbWndExtra = 0;
		wcex.hInstance = hInstance;
		wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
		wcex.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
		wcex.lpszClassName = L"WinWebDiffWindowClass";
		return RegisterClassExW(&wcex);
	}

	int m_nPanes = 0;
	HWND m_hWnd = nullptr;
	HINSTANCE m_hInstance = nullptr;
	CWebWindow m_webWindow[3];
	int m_nDraggingSplitter = -1;
	bool m_bHorizontalSplit = false;
	bool m_bDragging = false;
	POINT m_ptOrg{};
	POINT m_ptPrev{};
	SIZE m_size{ 1024, 600 };
	bool m_fitToWindow = true;
	double m_zoom = 1.0;
	std::wstring m_userAgent = L"";
	UserdataFolderType m_userDataFolderType = UserdataFolderType::APPDATA;
	bool m_bUserDataFolderPerPane = true;
	std::vector<ComPtr<IWebDiffEventHandler>> m_listeners;
	int m_currentDiffIndex = -1;
	std::vector<DiffInfo> m_diffInfoList;
	DiffOptions m_diffOptions{};
	int m_rootNodeId[3] = { -1, -1, -1 };
	bool m_bShowDifferences = true;
	COLORREF m_selDiffColor = RGB(0xff, 0x40, 0x40);
	COLORREF m_selDiffDeletedColor = RGB(0xf0, 0xc0, 0xc0);
	COLORREF m_diffColor = RGB(0xff, 0xff, 0x40);
	COLORREF m_diffDeletedColor = RGB(0xc0, 0xc0, 0xc0);
};
