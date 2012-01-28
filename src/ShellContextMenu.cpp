// grepWin - regex search and replace for Windows

// Copyright (C) 2007-2012 - Stefan Kueng

// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//
#include "stdafx.h"
#include "ShellContextMenu.h"
#include "shellapi.h"
#include "StringUtils.h"
#include "Registry.h"
#include "SearchInfo.h"
#include "LineData.h"
#include <algorithm>

#define MIN_ID 6
#define MAX_ID 10000

IContextMenu2 * g_IContext2 = NULL;
IContextMenu3 * g_IContext3 = NULL;
WNDPROC g_OldWndProc = NULL;

CShellContextMenu::CShellContextMenu() 
    : m_pFolderhook(NULL)
    , m_psfFolder(NULL)
    , m_pidlArray(NULL)
    , m_pidlArrayItems(0)
    , m_Menu(NULL)
{
}

CShellContextMenu::~CShellContextMenu()
{
    // free all allocated data
    delete m_pFolderhook;
    if (m_psfFolder && bDelete)
        m_psfFolder->Release ();
    m_psfFolder = NULL;
    FreePIDLArray (m_pidlArray, m_pidlArrayItems);
    m_pidlArray = NULL;
    m_pidlArrayItems = 0;

    if (m_Menu)
        DestroyMenu(m_Menu);
}


// this functions determines which version of IContextMenu is available for those objects (always the highest one)
// and returns that interface
BOOL CShellContextMenu::GetContextMenu(HWND hWnd, void ** ppContextMenu, int & iMenuType)
{
    *ppContextMenu = NULL;
    LPCONTEXTMENU icm1 = NULL;

    if (m_psfFolder == NULL)
        return FALSE;

    HKEY ahkeys[16] = {0};
    int numkeys = 0;
    if (RegOpenKey(HKEY_CLASSES_ROOT, L"*", &ahkeys[numkeys++]) != ERROR_SUCCESS)
        numkeys--;
    if (RegOpenKey(HKEY_CLASSES_ROOT, L"AllFileSystemObjects", &ahkeys[numkeys++]) != ERROR_SUCCESS)
        numkeys--;
    if (PathIsDirectory(m_strVector[0].filepath.c_str()))
    {
        if (RegOpenKey(HKEY_CLASSES_ROOT, L"Folder", &ahkeys[numkeys++]) != ERROR_SUCCESS)
            numkeys--;
        if (RegOpenKey(HKEY_CLASSES_ROOT, L"Directory", &ahkeys[numkeys++]) != ERROR_SUCCESS)
            numkeys--;
    }
    // find extension
    size_t dotpos = m_strVector[0].filepath.find_last_of('.');
    wstring ext;
    if (dotpos != std::string::npos)
    {
        ext = m_strVector[0].filepath.substr(dotpos);
        if (RegOpenKey(HKEY_CLASSES_ROOT, ext.c_str(), &ahkeys[numkeys++]) == ERROR_SUCCESS)
        {
            WCHAR buf[MAX_PATH] = {0};
            DWORD dwSize = MAX_PATH;
            if (RegQueryValueEx(ahkeys[numkeys-1], L"", NULL, NULL, (LPBYTE)buf, &dwSize) == ERROR_SUCCESS)
            {
                if (RegOpenKey(HKEY_CLASSES_ROOT, buf, &ahkeys[numkeys++]) != ERROR_SUCCESS)
                    numkeys--;
            }
        }
    }

    delete m_pFolderhook;
    m_pFolderhook = new CIShellFolderHook(m_psfFolder, this);

    CDefFolderMenu_Create2(NULL, hWnd, (UINT)m_pidlArrayItems, (LPCITEMIDLIST*)m_pidlArray, m_pFolderhook, dfmCallback, numkeys, ahkeys, &icm1);
    for (int i = 0; i < numkeys; ++i)
        RegCloseKey(ahkeys[i]);

    if (icm1)
    {   // since we got an IContextMenu interface we can now obtain the higher version interfaces via that
        if (icm1->QueryInterface(IID_IContextMenu3, ppContextMenu) == S_OK)
            iMenuType = 3;
        else if (icm1->QueryInterface(IID_IContextMenu2, ppContextMenu) == S_OK)
            iMenuType = 2;

        if (*ppContextMenu)
            icm1->Release(); // we can now release version 1 interface, cause we got a higher one
        else
        {
            // since no higher versions were found
            // redirect ppContextMenu to version 1 interface
            iMenuType = 1;
            *ppContextMenu = icm1;
        }
    }
    else
        return FALSE;

    return TRUE;
}


LRESULT CALLBACK CShellContextMenu::HookWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_MENUCHAR:   // only supported by IContextMenu3
        if (g_IContext3)
        {
            LRESULT lResult = 0;
            g_IContext3->HandleMenuMsg2 (message, wParam, lParam, &lResult);
            return (lResult);
        }
        break;

    case WM_DRAWITEM:
    case WM_MEASUREITEM:
        if (wParam)
            break; // if wParam != 0 then the message is not menu-related

    case WM_INITMENU:
    case WM_INITMENUPOPUP:
        if (g_IContext3)
        {
            LRESULT lResult = 0;
            g_IContext3->HandleMenuMsg2 (message, wParam, lParam, &lResult);
        }
        else
            g_IContext2->HandleMenuMsg (message, wParam, lParam);

        return TRUE;
        break;

    default:
        break;
    }

    // call original WndProc of window to prevent undefined behavior of window
    return ::CallWindowProc (g_OldWndProc , hWnd, message, wParam, lParam);
}


UINT CShellContextMenu::ShowContextMenu(HWND hWnd, POINT pt)
{
    int iMenuType = 0;  // to know which version of IContextMenu is supported
    LPCONTEXTMENU pContextMenu; // common pointer to IContextMenu and higher version interface

    if (!GetContextMenu (hWnd, (void**)&pContextMenu, iMenuType))
        return 0;

    if (!m_Menu)
    {
        DestroyMenu(m_Menu);
        m_Menu = CreatePopupMenu();
    }

    CRegStdString regEditorCmd(L"Software\\grepWin\\editorcmd");
    std::wstring editorcmd = regEditorCmd;
    if (m_strVector.size() == 1)
    {
        if (editorcmd.size())
        {
            ::InsertMenu(m_Menu, 1, MF_BYPOSITION | MF_STRING, 5, _T("Open with Editor"));
            ::InsertMenu(m_Menu, 5, MF_SEPARATOR|MF_BYPOSITION, 0, NULL);
        }

        ::InsertMenu(m_Menu, 1, MF_BYPOSITION | MF_STRING, 1, _T("Open Containing Folder"));
        ::InsertMenu(m_Menu, 2, MF_BYPOSITION | MF_STRING, 2, _T("Copy path to clipboard"));
        ::InsertMenu(m_Menu, 3, MF_BYPOSITION | MF_STRING, 3, _T("Copy filename to clipboard"));
        if (m_lineVector.size())
            ::InsertMenu(m_Menu, 4, MF_BYPOSITION | MF_STRING, 4, _T("Copy text result to clipboard"));
        ::InsertMenu(m_Menu, 5, MF_SEPARATOR|MF_BYPOSITION, 0, NULL);
    }
    else if (m_strVector.size() > 1)
    {
        if (editorcmd.size())
        {
            ::InsertMenu(m_Menu, 1, MF_BYPOSITION | MF_STRING, 5, _T("Open with Editor"));
            ::InsertMenu(m_Menu, 5, MF_SEPARATOR|MF_BYPOSITION, 0, NULL);
        }
        ::InsertMenu(m_Menu, 2, MF_BYPOSITION | MF_STRING, 2, _T("Copy paths to clipboard"));
        ::InsertMenu(m_Menu, 3, MF_BYPOSITION | MF_STRING, 3, _T("Copy filenames to clipboard"));
        if (m_lineVector.size())
            ::InsertMenu(m_Menu, 4, MF_BYPOSITION | MF_STRING, 4, _T("Copy text results to clipboard"));
        ::InsertMenu(m_Menu, 5, MF_SEPARATOR|MF_BYPOSITION, 0, NULL);
    }
    // lets fill the our popup menu
    pContextMenu->QueryContextMenu(m_Menu, GetMenuItemCount(m_Menu), MIN_ID, MAX_ID, CMF_NORMAL | CMF_EXPLORE);

    // subclass window to handle menu related messages in CShellContextMenu
    if (iMenuType > 1)  // only subclass if its version 2 or 3
    {
        g_OldWndProc = (WNDPROC)SetWindowLongPtr (hWnd, GWLP_WNDPROC, (LONG_PTR)HookWndProc);
        if (iMenuType == 2)
            g_IContext2 = (LPCONTEXTMENU2)pContextMenu;
        else    // version 3
            g_IContext3 = (LPCONTEXTMENU3)pContextMenu;
    }
    else
        g_OldWndProc = NULL;

    UINT idCommand = TrackPopupMenu(m_Menu, TPM_RETURNCMD | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, NULL);

    if (g_OldWndProc) // un-subclass
        SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR) g_OldWndProc);

    if (idCommand >= MIN_ID && idCommand <= MAX_ID) // see if returned idCommand belongs to shell menu entries
    {
        InvokeCommand(pContextMenu, idCommand - MIN_ID);    // execute related command
        idCommand = 0;
    }
    else
    {
        switch (idCommand)
        {
        case 1:
            {
                // This is the command line for explorer which tells it to select the given file
                wstring sFolder = _T( "/Select,\"" ) + m_strVector[0].filepath + _T("\"");

                // Prepare shell execution params
                SHELLEXECUTEINFO shExecInfo   = { 0 };
                shExecInfo.cbSize             = sizeof(shExecInfo);
                shExecInfo.lpFile             = _T("explorer.exe");
                shExecInfo.lpParameters       = sFolder.c_str();
                shExecInfo.nShow              = SW_SHOWNORMAL;
                shExecInfo.lpVerb             = _T("open"); // Context menu item
                shExecInfo.fMask              = SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_DDEWAIT | SEE_MASK_FLAG_NO_UI;

                // Select file in explorer
                ShellExecuteEx(&shExecInfo);
            }
            break;
        case 2:
            {
                wstring pathnames;
                for (auto it = m_strVector.begin(); it != m_strVector.end(); ++it)
                {
                    if (pathnames.size())
                        pathnames += _T("\r\n");
                    pathnames += it->filepath;
                }
                WriteAsciiStringToClipboard(pathnames.c_str(), hWnd);
            }
            break;
        case 3:
            {
                wstring pathnames;
                for (auto it = m_strVector.begin(); it != m_strVector.end(); ++it)
                {
                    if (pathnames.size())
                        pathnames += _T("\r\n");
                    wstring p = it->filepath;
                    p = p.substr(p.find_last_of('\\')+1);
                    pathnames += p;
                }
                WriteAsciiStringToClipboard(pathnames.c_str(), hWnd);
            }
            break;
        case 4:
            {
                wstring lines;
                for (auto it = m_lineVector.begin(); it != m_lineVector.end(); ++it)
                {
                    if (lines.size())
                        lines += _T("\r\n");
                    for (auto it2 = it->lines.cbegin(); it2 != it->lines.cend(); ++it2)
                    {
                        wstring l = it2->text;
                        std::replace(l.begin(), l.end(), '\n', ' ');
                        std::replace(l.begin(), l.end(), '\r', ' ');

                        lines += l;
                    }
                }
                WriteAsciiStringToClipboard(lines.c_str(), hWnd);
            }
            break;
        case 5:
            {
                if (m_lineVector.size())
                {
                    for (auto it = m_lineVector.cbegin(); it != m_lineVector.cend(); ++it)
                    {
                        for (auto it2 = it->lines.cbegin(); it2 != it->lines.cend(); ++it2)
                        {
                            std::wstring cmd = regEditorCmd;
                            SearchReplace(cmd, L"%path%", it->path.c_str());
                            wchar_t buf[40] = {0};
                            swprintf_s(buf, L"%ld", it2->number);
                            SearchReplace(cmd, L"%line%", buf);

                            STARTUPINFO startupInfo;
                            PROCESS_INFORMATION processInfo;
                            memset(&startupInfo, 0, sizeof(STARTUPINFO));
                            startupInfo.cb = sizeof(STARTUPINFO);
                            memset(&processInfo, 0, sizeof(PROCESS_INFORMATION));
                            CreateProcess(NULL, const_cast<TCHAR*>(cmd.c_str()), NULL, NULL, FALSE, 0, 0, NULL, &startupInfo, &processInfo);
                            CloseHandle(processInfo.hThread);
                            CloseHandle(processInfo.hProcess);
                        }
                    }
                }
                else
                {
                    for (auto it = m_strVector.begin(); it != m_strVector.end(); ++it)
                    {
                        std::wstring cmd = regEditorCmd;
                        SearchReplace(cmd, L"%path%", it->filepath.c_str());
                        if (it->matchlinesnumbers.size())
                        {
                            wchar_t buf[40] = {0};
                            swprintf_s(buf, L"%ld", it->matchlinesnumbers[0]);
                            SearchReplace(cmd, L"%line%", buf);
                        }
                        else
                            SearchReplace(cmd, L"%line%", L"0");

                        STARTUPINFO startupInfo;
                        PROCESS_INFORMATION processInfo;
                        memset(&startupInfo, 0, sizeof(STARTUPINFO));
                        startupInfo.cb = sizeof(STARTUPINFO);
                        memset(&processInfo, 0, sizeof(PROCESS_INFORMATION));
                        CreateProcess(NULL, const_cast<TCHAR*>(cmd.c_str()), NULL, NULL, FALSE, 0, 0, NULL, &startupInfo, &processInfo);
                        CloseHandle(processInfo.hThread);
                        CloseHandle(processInfo.hProcess);
                    }
                }
            }
            break;
        }
    }

    pContextMenu->Release();
    g_IContext2 = NULL;
    g_IContext3 = NULL;
    delete m_pFolderhook;
    m_pFolderhook = NULL;
    return (idCommand);
}


void CShellContextMenu::InvokeCommand(LPCONTEXTMENU pContextMenu, UINT idCommand)
{
    CMINVOKECOMMANDINFO cmi = {0};
    cmi.cbSize = sizeof (CMINVOKECOMMANDINFO);
    cmi.lpVerb = (LPSTR) MAKEINTRESOURCE (idCommand);
    cmi.nShow = SW_SHOWNORMAL;

    pContextMenu->InvokeCommand (&cmi);
}

void CShellContextMenu::SetObjects(const vector<CSearchInfo>& strVector, const vector<LineData>& lineVector)
{
    // free all allocated data
    if (m_psfFolder && bDelete)
        m_psfFolder->Release();
    m_psfFolder = NULL;
    FreePIDLArray(m_pidlArray, m_pidlArrayItems);
    m_pidlArray = NULL;

    // get IShellFolder interface of Desktop (root of shell namespace)
    SHGetDesktopFolder(&m_psfFolder);    // needed to obtain full qualified pidl

    // ParseDisplayName creates a PIDL from a file system path relative to the IShellFolder interface
    // but since we use the Desktop as our interface and the Desktop is the namespace root
    // that means that it's a fully qualified PIDL, which is what we need
    LPITEMIDLIST pidl = NULL;

    m_psfFolder->ParseDisplayName(NULL, 0, (LPWSTR)strVector[0].filepath.c_str(), NULL, &pidl, NULL);

    // get interface to IMalloc (need to free the PIDLs allocated by the shell functions)
    CoTaskMemFree(pidl);

    nItems = (int)strVector.size();
    m_pidlArray = (LPITEMIDLIST *)CoTaskMemAlloc((nItems + 10) * sizeof (LPITEMIDLIST));
    SecureZeroMemory(m_pidlArray, (nItems + 10) * sizeof (LPITEMIDLIST));
    m_pidlArrayItems = nItems;
    for (int i = 0; i < nItems; i++)
    {
        if (SUCCEEDED(m_psfFolder->ParseDisplayName(NULL, 0, (LPWSTR)strVector[i].filepath.c_str(), NULL, &pidl, NULL)))
        {
            m_pidlArray[i] = CopyPIDL(pidl);    // copy pidl to pidlArray
            CoTaskMemFree(pidl);                // free pidl allocated by ParseDisplayName
        }
    }

    m_strVector = strVector;
    m_lineVector = lineVector;
    bDelete = TRUE; // indicates that m_psfFolder should be deleted by CShellContextMenu
}

void CShellContextMenu::FreePIDLArray(LPITEMIDLIST *pidlArray, int nItems)
{
    if (!pidlArray)
        return;

    for (int i = 0; i < nItems; i++)
        CoTaskMemFree(pidlArray[i]);
    CoTaskMemFree(pidlArray);
}


LPITEMIDLIST CShellContextMenu::CopyPIDL(LPCITEMIDLIST pidl, int cb)
{
    if (cb == -1)
        cb = GetPIDLSize(pidl); // Calculate size of list.

    LPITEMIDLIST pidlRet = (LPITEMIDLIST)CoTaskMemAlloc ((cb + sizeof(USHORT)) * sizeof(BYTE));
    SecureZeroMemory(pidlRet, (cb + sizeof(USHORT)) * sizeof(BYTE));
    if (pidlRet)
        CopyMemory(pidlRet, pidl, cb);

    return pidlRet;
}


UINT CShellContextMenu::GetPIDLSize(LPCITEMIDLIST pidl)
{
    if (!pidl)
        return 0;
    int nSize = 0;
    LPITEMIDLIST pidlTemp = (LPITEMIDLIST)pidl;
    while (pidlTemp->mkid.cb)
    {
        nSize += pidlTemp->mkid.cb;
        pidlTemp = (LPITEMIDLIST)(((LPBYTE)pidlTemp) + pidlTemp->mkid.cb);
    }
    return nSize;
}

HMENU CShellContextMenu::GetMenu()
{
    if (!m_Menu)
    {
        m_Menu = CreatePopupMenu();
    }
    return m_Menu;
}

LPBYTE CShellContextMenu::GetPIDLPos(LPCITEMIDLIST pidl, int nPos)
{
    if (!pidl)
        return 0;
    int nCount = 0;

    BYTE * pCur = (BYTE *)pidl;
    while (((LPCITEMIDLIST)pCur)->mkid.cb)
    {
        if (nCount == nPos)
            return pCur;
        nCount++;
        pCur += ((LPCITEMIDLIST)pCur)->mkid.cb;
    }
    if (nCount == nPos)
        return pCur;
    return NULL;
}


int CShellContextMenu::GetPIDLCount(LPCITEMIDLIST pidl)
{
    if (!pidl)
        return 0;

    int nCount = 0;
    BYTE*  pCur = (BYTE *)pidl;
    while (((LPCITEMIDLIST)pCur)->mkid.cb)
    {
        nCount++;
        pCur += ((LPCITEMIDLIST)pCur)->mkid.cb;
    }
    return nCount;
}

HRESULT CShellContextMenu::dfmCallback( IShellFolder * /*psf*/, HWND /*hwnd*/, IDataObject * /*pdtobj*/, UINT uMsg, WPARAM /*wParam*/, LPARAM /*lParam*/ )
{
    switch (uMsg)
    {
    case DFM_MERGECONTEXTMENU:
        return S_OK;
    case DFM_INVOKECOMMAND:
    case DFM_INVOKECOMMANDEX:
    case DFM_GETDEFSTATICID: // Required for Windows 7 to pick a default
        return S_FALSE;
    }
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE CIShellFolderHook::GetUIObjectOf( HWND hwndOwner, UINT cidl, LPCITEMIDLIST *apidl, REFIID riid, UINT *rgfReserved, void **ppv )
{
    if(InlineIsEqualGUID(riid, IID_IDataObject))
    {
        HRESULT hres = m_iSF->GetUIObjectOf(hwndOwner, cidl, apidl, IID_IDataObject, NULL, ppv);
        if (FAILED(hres))
            return hres;

        IDataObject * idata = (LPDATAOBJECT)(*ppv);
        // the IDataObject returned here doesn't have a HDROP, so we create one ourselves and add it to the IDataObject
        // the HDROP is necessary for most context menu handlers

        int nLength = 0;
        for (size_t i=0;i<m_pShellContextMenu->m_strVector.size();i++)
        {
            nLength += (int)m_pShellContextMenu->m_strVector[i].filepath.size();
            nLength += 1; // '\0' separator
        }
        int nBufferSize = sizeof(DROPFILES) + ((nLength+3)*sizeof(TCHAR));
        char * pBuffer = new char[nBufferSize];
        SecureZeroMemory(pBuffer, nBufferSize);
        DROPFILES* df = (DROPFILES*)pBuffer;
        df->pFiles = sizeof(DROPFILES);
        df->fWide = 1;
        TCHAR* pFilenames = (TCHAR*)(pBuffer + sizeof(DROPFILES));
        TCHAR* pCurrentFilename = pFilenames;

        for (size_t i=0;i<m_pShellContextMenu->m_strVector.size();i++)
        {
            wstring str = m_pShellContextMenu->m_strVector[i].filepath;
            wcscpy_s(pCurrentFilename, str.size()+1, str.c_str());
            pCurrentFilename += str.size();
            *pCurrentFilename = '\0'; // separator between file names
            pCurrentFilename++;
        }
        *pCurrentFilename = '\0'; // terminate array
        pCurrentFilename++;
        *pCurrentFilename = '\0'; // terminate array
        STGMEDIUM * pmedium = new STGMEDIUM;
        pmedium->tymed = TYMED_HGLOBAL;
        pmedium->hGlobal = GlobalAlloc(GMEM_ZEROINIT|GMEM_MOVEABLE, nBufferSize+20);
        if (pmedium->hGlobal)
        {
            LPVOID pMem = ::GlobalLock(pmedium->hGlobal);
            if (pMem)
                memcpy(pMem, pBuffer, nBufferSize);
            GlobalUnlock(pmedium->hGlobal);
            FORMATETC formatetc = {0};
            formatetc.cfFormat = CF_HDROP;
            formatetc.dwAspect = DVASPECT_CONTENT;
            formatetc.lindex = -1;
            formatetc.tymed = TYMED_HGLOBAL;
            pmedium->pUnkForRelease = NULL;
            hres = idata->SetData(&formatetc, pmedium, TRUE);
            delete [] pBuffer;
            return hres;
        }
        delete [] pBuffer;
        return E_OUTOFMEMORY;
    }
    else 
    {
        // just pass it on to the base object
        return m_iSF->GetUIObjectOf(hwndOwner, cidl, apidl, riid, rgfReserved, ppv);
    }
}

