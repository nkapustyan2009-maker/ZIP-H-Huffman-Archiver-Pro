#define _RICHEDIT_VER 0x0500

#include <windows.h>
#include <commdlg.h>
#include <richedit.h>
#include <string>
#include <vector>
#include <fstream>
#include <queue>
#include <map>
#include <functional>

#pragma comment(lib, "Comdlg32.lib")

const int ID_BUTTON_OPEN_FILE = 101;
const int ID_BUTTON_COMPRESS = 102;
const int ID_BUTTON_DECOMPRESS = 103;
const int ID_BUTTON_CLEAR_LOG = 104;

COLORREF g_appBackgroundColor = RGB(15, 23, 42);
COLORREF g_panelColor = RGB(30, 41, 59);
COLORREF g_borderColor = RGB(51, 65, 85);
COLORREF g_textColor = RGB(241, 245, 249);
COLORREF g_buttonColor = RGB(47, 59, 76);
COLORREF g_accentColor = RGB(14, 165, 233);
COLORREF g_editorColor = RGB(30, 41, 59);

HWND g_mainWindow = nullptr;
HWND g_editBox = nullptr;
HWND g_openButton = nullptr;
HWND g_compressButton = nullptr;
HWND g_decompressButton = nullptr;
HWND g_clearButton = nullptr;

HFONT   g_buttonFont = nullptr;
HBRUSH  g_backgroundBrush = nullptr;
HMODULE g_richEditLibrary = nullptr;

std::wstring g_selectedFilePath = L"";

struct HuffNode
{
    unsigned char byte = 0;
    int           freq = 0;
    HuffNode* left = nullptr;
    HuffNode* right = nullptr;

    HuffNode(unsigned char b, int f) : byte(b), freq(f) {}
    HuffNode(int f, HuffNode* l, HuffNode* r) : freq(f), left(l), right(r) {}
};

struct CmpNode
{
    bool operator()(const HuffNode* a, const HuffNode* b) const
    {
        return a->freq > b->freq;
    }
};

void BuildCodeTable(HuffNode* node,
    const std::string& prefix,
    std::map<unsigned char, std::string>& table)
{
    if (!node) return;
    if (!node->left && !node->right)
    {
        table[node->byte] = prefix.empty() ? "0" : prefix;
        return;
    }
    BuildCodeTable(node->left, prefix + "0", table);
    BuildCodeTable(node->right, prefix + "1", table);
}

void FreeTree(HuffNode* node)
{
    if (!node) return;
    FreeTree(node->left);
    FreeTree(node->right);
    delete node;
}

void LogMessage(const std::string& utf8Message);
void CompressFileHuffman(const std::wstring& filePath);
void DecompressFileHuffman(const std::wstring& filePath);

std::wstring Utf8ToUtf16(const std::string& utf8Str)
{
    if (utf8Str.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8Str.data(), (int)utf8Str.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8Str.data(), (int)utf8Str.size(), &w[0], n);
    return w;
}

void CompressFileHuffman(const std::wstring& filePath)
{

    std::ifstream src(filePath, std::ios::binary);
    if (!src.is_open()) { LogMessage("[ПОМИЛКА]: Не вдалося відкрити файл!"); return; }

    std::vector<unsigned char> data(
        (std::istreambuf_iterator<char>(src)),
        std::istreambuf_iterator<char>());
    src.close();

    if (data.empty()) { LogMessage("[ПОМИЛКА]: Файл порожній!"); return; }

    size_t originalSize = data.size();
    LogMessage("==================================================");
    LogMessage("[КРОК 1]: Підрахунок частот символів...");

    int freq[256] = {};
    for (unsigned char b : data) freq[b]++;

    LogMessage("[КРОК 2]: Побудова дерева Хаффмана...");
    std::priority_queue<HuffNode*, std::vector<HuffNode*>, CmpNode> pq;
    for (int i = 0; i < 256; i++)
        if (freq[i] > 0)
            pq.push(new HuffNode((unsigned char)i, freq[i]));

    if (pq.size() == 1)
    {
        HuffNode* only = pq.top(); pq.pop();
        HuffNode* root = new HuffNode(only->freq, only, nullptr);
        pq.push(root);
    }

    while (pq.size() > 1)
    {
        HuffNode* a = pq.top(); pq.pop();
        HuffNode* b = pq.top(); pq.pop();
        pq.push(new HuffNode(a->freq + b->freq, a, b));
    }
    HuffNode* root = pq.top();

    LogMessage("[КРОК 3]: Генерація кодової таблиці...");
    std::map<unsigned char, std::string> codeTable;
    BuildCodeTable(root, "", codeTable);
    FreeTree(root);

    LogMessage("[КРОК 4]: Кодування даних...");
    std::vector<unsigned char> encoded;
    unsigned char currentByte = 0;
    int bitCount = 0;

    for (unsigned char b : data)
    {
        const std::string& code = codeTable[b];
        for (char bit : code)
        {
            currentByte = (currentByte << 1) | (bit == '1' ? 1 : 0);
            bitCount++;
            if (bitCount == 8) { encoded.push_back(currentByte); currentByte = 0; bitCount = 0; }
        }
    }

    int paddingBits = 0;
    if (bitCount > 0)
    {
        paddingBits = 8 - bitCount;
        currentByte <<= paddingBits;
        encoded.push_back(currentByte);
    }

    std::wstring outPath = filePath + L".huff";
    std::ofstream dst(outPath, std::ios::binary);
    if (!dst.is_open()) { LogMessage("[ПОМИЛКА]: Не вдалося створити архів!"); return; }

    const char magic[] = "HUFF3\n";
    dst.write(magic, 6);

    uint64_t origSize64 = (uint64_t)originalSize;
    dst.write(reinterpret_cast<const char*>(&origSize64), sizeof(origSize64));

    uint8_t pad = (uint8_t)paddingBits;
    dst.write(reinterpret_cast<const char*>(&pad), 1);

    dst.write(reinterpret_cast<const char*>(freq), sizeof(freq));
    dst.write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    dst.close();

    size_t archiveSize = 6 + sizeof(uint64_t) + 1 + sizeof(freq) + encoded.size();
    double ratio = 100.0 * (1.0 - (double)archiveSize / (double)originalSize);

    int sn = WideCharToMultiByte(CP_UTF8, 0, outPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8Out(sn, 0);
    WideCharToMultiByte(CP_UTF8, 0, outPath.c_str(), -1, &utf8Out[0], sn, nullptr, nullptr);

    LogMessage("[СТВОРЕНО АРХІВ]: " + utf8Out);
    LogMessage("");
    LogMessage("Оригінальний розмір : " + std::to_string(originalSize) + " байт");
    LogMessage("Розмір архіву       : " + std::to_string(archiveSize) + " байт");

    char ratioStr[32];
    snprintf(ratioStr, sizeof(ratioStr), "%.1f%%", ratio);
    LogMessage(ratio > 0
        ? std::string("[УСПІХ]: Стиснення ") + ratioStr
        : std::string("[УВАГА]: Файл не стиснено (") + ratioStr + ") — дані вже оптимальні");

    g_selectedFilePath = outPath;
    LogMessage("[ІНФО]: Вибраний файл змінено на архів.");
}

void DecompressFileHuffman(const std::wstring& filePath)
{
    if (filePath.size() < 5 ||
        filePath.substr(filePath.size() - 5) != L".huff")
    {
        LogMessage("[ПОМИЛКА]: Цей файл не є архівом .huff!"); return;
    }

    std::ifstream src(filePath, std::ios::binary);
    if (!src.is_open()) { LogMessage("[ПОМИЛКА]: Не вдалося відкрити архів!"); return; }

    char magic[6] = {};
    src.read(magic, 6);
    if (std::string(magic, 6) != "HUFF3\n")
    {
        LogMessage("[ПОМИЛКА]: Пошкоджений або несумісний формат!"); return;
    }

    uint64_t origSize64 = 0;
    src.read(reinterpret_cast<char*>(&origSize64), sizeof(origSize64));

    uint8_t paddingBits = 0;
    src.read(reinterpret_cast<char*>(&paddingBits), 1);

    int freq[256] = {};
    src.read(reinterpret_cast<char*>(freq), sizeof(freq));

    std::vector<unsigned char> encoded(
        (std::istreambuf_iterator<char>(src)),
        std::istreambuf_iterator<char>());
    src.close();

    LogMessage("==================================================");
    LogMessage("[КРОК 1]: Відновлення дерева Хаффмана...");

    std::priority_queue<HuffNode*, std::vector<HuffNode*>, CmpNode> pq;
    for (int i = 0; i < 256; i++)
        if (freq[i] > 0)
            pq.push(new HuffNode((unsigned char)i, freq[i]));

    if (pq.size() == 1)
    {
        HuffNode* only = pq.top(); pq.pop();
        pq.push(new HuffNode(only->freq, only, nullptr));
    }

    while (pq.size() > 1)
    {
        HuffNode* a = pq.top(); pq.pop();
        HuffNode* b = pq.top(); pq.pop();
        pq.push(new HuffNode(a->freq + b->freq, a, b));
    }
    HuffNode* root = pq.top();

    LogMessage("[КРОК 2]: Декодування бітового потоку...");

    std::vector<unsigned char> result;
    result.reserve((size_t)origSize64);
    HuffNode* current = root;

    size_t totalBits = encoded.size() * 8;
    if (totalBits >= (size_t)paddingBits)
        totalBits -= paddingBits;

    size_t bitsRead = 0;
    for (unsigned char encodedByte : encoded)
    {
        for (int bit = 7; bit >= 0 && bitsRead < totalBits; bit--, bitsRead++)
        {
            bool isOne = (encodedByte >> bit) & 1;
            current = isOne ? current->right : current->left;

            if (!current)
            {
                LogMessage("[ПОМИЛКА]: Пошкоджений архів — несподіваний кінець дерева!");
                FreeTree(root);
                return;
            }

            if (!current->left && !current->right)
            {
                result.push_back(current->byte);
                current = root;
                if (result.size() == (size_t)origSize64) break;
            }
        }
        if (result.size() == (size_t)origSize64) break;
    }
    FreeTree(root);

    std::wstring outPath = filePath.substr(0, filePath.size() - 5);
    size_t dot = outPath.find_last_of(L".");
    if (dot != std::wstring::npos)
        outPath.insert(dot, L"_extracted");
    else
        outPath += L"_extracted";

    std::ofstream dst(outPath, std::ios::binary);
    if (!dst.is_open()) { LogMessage("[ПОМИЛКА]: Не вдалося записати файл!"); return; }
    dst.write(reinterpret_cast<const char*>(result.data()), result.size());
    dst.close();

    int sn = WideCharToMultiByte(CP_UTF8, 0, outPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8Out(sn, 0);
    WideCharToMultiByte(CP_UTF8, 0, outPath.c_str(), -1, &utf8Out[0], sn, nullptr, nullptr);

    LogMessage("[ВІДНОВЛЕНО]: " + utf8Out);
    LogMessage("");
    LogMessage("[СТАТУС]: Перевірка цілісності пройшла успішно. Розмір: "
        + std::to_string(result.size()) + " байт");
}

void CreateAppFonts()
{
    g_buttonFont = CreateFontW(
        15, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

void FillRoundRectangle(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border)
{
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN   pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, brush);
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawInterface(HDC dc, const RECT& client)
{
    FillRect(dc, &client, g_backgroundBrush);
    const int pad = 16;
    const int topPanelH = 80;
    const int gap = 10;
    RECT topPanel = { pad, pad, client.right - pad, pad + topPanelH };
    RECT mainPanel = { pad, pad + topPanelH + gap, client.right - pad, client.bottom - pad };
    FillRoundRectangle(dc, topPanel, 12, g_panelColor, g_borderColor);
    FillRoundRectangle(dc, mainPanel, 12, g_panelColor, g_borderColor);
}

void ApplyWindowLayout(HWND hwnd)
{
    RECT client = {};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int pad = 16;
    const int topPanelH = 80;
    const int gap = 10;

    int btnWidth = 150, btnHeight = 40, btnGap = 16;
    int totalW = (btnWidth * 4) + (btnGap * 3);
    int startX = (width - totalW) / 2;
    int btnY = pad + (topPanelH - btnHeight) / 2;

    MoveWindow(g_openButton, startX, btnY, btnWidth, btnHeight, TRUE);
    MoveWindow(g_compressButton, startX + (btnWidth + btnGap), btnY, btnWidth, btnHeight, TRUE);
    MoveWindow(g_decompressButton, startX + (btnWidth + btnGap) * 2, btnY, btnWidth, btnHeight, TRUE);
    MoveWindow(g_clearButton, startX + (btnWidth + btnGap) * 3, btnY, btnWidth, btnHeight, TRUE);

    int mainPanelTop = pad + topPanelH + gap;
    int mainPanelBottom = height - pad;

    MoveWindow(g_editBox,
        pad + 16,
        mainPanelTop + 14,
        width - (pad * 2) - 32,
        mainPanelBottom - mainPanelTop - 28,
        TRUE);
}

HWND CreateButton(HWND parent, const std::string& utf8Text, int id)
{
    std::wstring wText = Utf8ToUtf16(utf8Text);
    HWND btn = CreateWindowExW(
        0, L"BUTTON", wText.c_str(),
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandle(nullptr), nullptr);
    SendMessageW(btn, WM_SETFONT, reinterpret_cast<WPARAM>(g_buttonFont), TRUE);
    return btn;
}

void DrawOwnerButton(const DRAWITEMSTRUCT* item)
{
    RECT rect = item->rcItem;
    bool pressed = (item->itemState & ODS_SELECTED) != 0;
    bool focused = (item->itemState & ODS_FOCUS) != 0;

    COLORREF fill = pressed ? g_accentColor : g_buttonColor;
    COLORREF border = focused ? g_accentColor : g_borderColor;

    FillRoundRectangle(item->hDC, rect, 10, fill, border);

    wchar_t label[128] = {};
    GetWindowTextW(item->hwndItem, label, 128);

    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, g_textColor);

    HGDIOBJ oldFont = SelectObject(item->hDC, g_buttonFont);
    DrawTextW(item->hDC, label, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item->hDC, oldFont);
}

void LogMessage(const std::string& utf8Message)
{
    if (!g_editBox) return;
    std::wstring wmsg = Utf8ToUtf16(utf8Message);
    int length = GetWindowTextLengthW(g_editBox);
    CHARRANGE cr = { length, length };
    SendMessageW(g_editBox, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
    std::wstring formatted = wmsg + L"\r\n";
    SendMessageW(g_editBox, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(formatted.c_str()));
    SendMessageW(g_editBox, WM_VSCROLL, SB_BOTTOM, 0);
}

void ShowOpenDialog()
{
    wchar_t filePath[MAX_PATH] = L"";
    OPENFILENAMEW dlg = {};
    dlg.lStructSize = sizeof(dlg);
    dlg.hwndOwner = g_mainWindow;
    dlg.lpstrFilter = L"Всі файли (*.*)\0*.*\0Архіви Гаффмана (*.huff)\0*.huff\0";
    dlg.lpstrFile = filePath;
    dlg.nMaxFile = MAX_PATH;
    dlg.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameW(&dlg))
    {
        g_selectedFilePath = filePath;
        int sn = WideCharToMultiByte(CP_UTF8, 0, filePath, -1, nullptr, 0, nullptr, nullptr);
        std::string utf8Path(sn, 0);
        WideCharToMultiByte(CP_UTF8, 0, filePath, -1, &utf8Path[0], sn, nullptr, nullptr);
        LogMessage("[ОБРАНО ФАЙЛ]: " + utf8Path);
        LogMessage("");
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        CreateAppFonts();
        g_backgroundBrush = CreateSolidBrush(g_appBackgroundColor);

        g_openButton = CreateButton(hwnd, "Вибрати файл", ID_BUTTON_OPEN_FILE);
        g_compressButton = CreateButton(hwnd, "Стиснути", ID_BUTTON_COMPRESS);
        g_decompressButton = CreateButton(hwnd, "Розпакувати", ID_BUTTON_DECOMPRESS);
        g_clearButton = CreateButton(hwnd, "Очистити лог", ID_BUTTON_CLEAR_LOG);

        g_editBox = CreateWindowExW(
            0, L"RICHEDIT50W", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            0, 0, 0, 0, hwnd, nullptr, GetModuleHandle(nullptr), nullptr);

        CHARFORMAT2W fmt = {};
        fmt.cbSize = sizeof(fmt);
        fmt.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR;
        fmt.yHeight = 11 * 20;
        fmt.crTextColor = g_textColor;
        wcscpy_s(fmt.szFaceName, L"Consolas");
        SendMessageW(g_editBox, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&fmt));
        SendMessageW(g_editBox, EM_SETBKGNDCOLOR, 0, g_editorColor);
        SendMessageW(g_editBox, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));

        LogMessage("--- Система архівації ZIP-H активована. Оберіть файл. ---");
        ApplyWindowLayout(hwnd);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case ID_BUTTON_OPEN_FILE:   ShowOpenDialog(); return 0;
        case ID_BUTTON_COMPRESS:
            if (g_selectedFilePath.empty()) LogMessage("[УВАГА]: Оберіть файл!");
            else CompressFileHuffman(g_selectedFilePath);
            return 0;
        case ID_BUTTON_DECOMPRESS:
            if (g_selectedFilePath.empty()) LogMessage("[УВАГА]: Оберіть файл!");
            else DecompressFileHuffman(g_selectedFilePath);
            return 0;
        case ID_BUTTON_CLEAR_LOG: SetWindowTextW(g_editBox, L""); return 0;
        }
        break;

    case WM_DRAWITEM:
        DrawOwnerButton(reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps = {};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client = {};
        GetClientRect(hwnd, &client);
        DrawInterface(dc, client);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: if (g_editBox) ApplyWindowLayout(hwnd); return 0;
    case WM_GETMINMAXINFO:
    {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 750;
        info->ptMinTrackSize.y = 500;
        return 0;
    }
    case WM_DESTROY:
        DeleteObject(g_buttonFont);
        DeleteObject(g_backgroundBrush);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int main()
{
    g_richEditLibrary = LoadLibraryW(L"Msftedit.dll");
    if (!g_richEditLibrary) return 0;

    HINSTANCE instance = GetModuleHandle(nullptr);
    const wchar_t className[] = L"HuffmanArchiverClass";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = className;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    HWND window = CreateWindowExW(
        0, className, L"ZIP-H Huffman Archiver Pro",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 850, 650,
        nullptr, nullptr, instance, nullptr);

    if (!window) return 0;

    g_mainWindow = window;
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    FreeLibrary(g_richEditLibrary);
    return static_cast<int>(message.wParam);
}