1   stdcall ParseURLA(str ptr)
2   stdcall ParseURLW(wstr ptr)
3   stdcall PathFileExistsDefExtA(str long)
4   stdcall PathFileExistsDefExtW(wstr long)
5   stdcall PathFindOnPathExA(str ptr long)
6   stdcall PathFindOnPathExW(wstr ptr long)
7   stdcall -ordinal SHAllocShared(ptr long long)
8   stdcall -ordinal SHLockShared(ptr long)
9   stdcall -ordinal SHUnlockShared(ptr)
10  stdcall -ordinal SHFreeShared(ptr long)
11  stdcall SHMapHandle(ptr long long long long)
12  stdcall SHCreateMemStream(ptr long)
13  stdcall RegisterDefaultAcceptHeaders(ptr ptr)
14  stdcall -ordinal GetAcceptLanguagesA(ptr ptr)
15  stdcall -ordinal GetAcceptLanguagesW(ptr ptr)
16  stdcall -ordinal SHCreateThread(ptr ptr long ptr)
17  stdcall SHWriteDataBlockList(ptr ptr)
18  stdcall SHReadDataBlockList(ptr ptr)
19  stdcall SHFreeDataBlockList(ptr)
20  stdcall SHAddDataBlock(ptr ptr)
21  stdcall SHRemoveDataBlock(ptr long)
22  stdcall SHFindDataBlock(ptr long)
23  stdcall SHStringFromGUIDA(ptr ptr long)
24  stdcall SHStringFromGUIDW(ptr ptr long)
25  stdcall IsCharAlphaWrapW(long) user32.IsCharAlphaW
26  stdcall IsCharUpperWrapW(long) user32.IsCharUpperW
27  stdcall IsCharLowerWrapW(long) user32.IsCharLowerW
28  stdcall IsCharAlphaNumericWrapW(long) user32.IsCharAlphaNumericW
29  stdcall -ordinal IsCharSpaceW(long)
30  stdcall IsCharBlankW(long)
31  stdcall IsCharPunctW(long)
32  stdcall IsCharCntrlW(ptr)
33  stdcall IsCharDigitW(long)
34  stdcall IsCharXDigitW(long)
35  stdcall GetStringType3ExW(ptr long ptr)
36  stdcall AppendMenuWrapW(long long long wstr) user32.AppendMenuW
37  stdcall CallWindowProcWrapW(ptr long long long long) user32.CallWindowProcW
38  stdcall CharLowerWrapW(wstr) user32.CharLowerW
39  stdcall CharLowerBuffWrapW(wstr long) user32.CharLowerBuffW
40  stdcall CharNextWrapW(wstr) user32.CharNextW
41  stdcall CharPrevWrapW(wstr wstr) user32.CharPrevW
42  stdcall CharToOemWrapW(wstr) user32.CharToOemW
43  stdcall CharUpperWrapW(wstr) user32.CharUpperW
44  stdcall CharUpperBuffWrapW(wstr long) user32.CharUpperBuffW
45  stdcall CompareStringWrapW(long long wstr long wstr long) kernel32.CompareStringW
46  stdcall CopyAcceleratorTableWrapW(long ptr long) user32.CopyAcceleratorTableW
47  stdcall CreateAcceleratorTableWrapW(ptr long) user32.CreateAcceleratorTableW
48  stdcall CreateDCWrapW(wstr wstr wstr ptr) gdi32.CreateDCW
49  stdcall CreateDialogParamWrapW(long ptr long ptr long) user32.CreateDialogParamW
50  stdcall CreateDirectoryWrapW(wstr ptr) kernel32.CreateDirectoryW
51  stdcall CreateEventWrapW(ptr long long wstr) kernel32.CreateEventW
52  stdcall CreateFileWrapW(wstr long long ptr long long long) kernel32.CreateFileW
53  stdcall CreateFontIndirectWrapW(ptr) gdi32.CreateFontIndirectW
54  stdcall CreateICWrapW(wstr wstr wstr ptr) gdi32.CreateICW
55  stdcall CreateWindowExWrapW(long wstr wstr long long long long long long long long ptr) user32.CreateWindowExW
56  stdcall DefWindowProcWrapW(long long long long) user32.DefWindowProcW
57  stdcall DeleteFileWrapW(wstr) kernel32.DeleteFileW
58  stdcall DialogBoxIndirectParamWrapW(long ptr long ptr long) user32.DialogBoxIndirectParamW
59  stdcall DialogBoxParamWrapW(long wstr long ptr long) user32.DialogBoxParamW
60  stdcall DispatchMessageWrapW(ptr) user32.DispatchMessageW
61  stdcall DrawTextWrapW(long wstr long ptr long) user32.DrawTextW
62  stdcall EnumFontFamiliesWrapW(long wstr ptr long) gdi32.EnumFontFamiliesW
63  stdcall EnumFontFamiliesExWrapW(long ptr ptr long long) gdi32.EnumFontFamiliesExW
64  stdcall EnumResourceNamesWrapW(long wstr ptr long) kernel32.EnumResourceNamesW
65  stdcall FindFirstFileWrapW(wstr ptr) kernel32.FindFirstFileW
66  stdcall FindResourceWrapW(long wstr wstr) kernel32.FindResourceW
67  stdcall FindWindowWrapW(wstr wstr) user32.FindWindowW
68  stdcall FormatMessageWrapW(long ptr long long ptr long ptr) kernel32.FormatMessageW
69  stdcall GetClassInfoWrapW(long wstr ptr) user32.GetClassInfoW
70  stdcall GetClassLongWrapW(long long) user32.GetClassLongW
71  stdcall GetClassNameWrapW(long ptr long) user32.GetClassNameW
72  stdcall GetClipboardFormatNameWrapW(long ptr long) user32.GetClipboardFormatNameW
73  stdcall GetCurrentDirectoryWrapW(long ptr) kernel32.GetCurrentDirectoryW
74  stdcall GetDlgItemTextWrapW(long long wstr long) user32.GetDlgItemTextW
75  stdcall GetFileAttributesWrapW(wstr) kernel32.GetFileAttributesW
76  stdcall GetFullPathNameWrapW(wstr long ptr ptr) kernel32.GetFullPathNameW
77  stdcall GetLocaleInfoWrapW(long long ptr long) kernel32.GetLocaleInfoW
78  stdcall GetMenuStringWrapW(long long ptr long long) user32.GetMenuStringW
79  stdcall GetMessageWrapW(ptr long long long) user32.GetMessageW
80  stdcall GetModuleFileNameWrapW(long ptr long) kernel32.GetModuleFileNameW
81  stdcall GetSystemDirectoryWrapW(ptr long) kernel32.GetSystemDirectoryW
82  stdcall SearchPathWrapW(wstr wstr wstr long ptr ptr) kernel32.SearchPathW
83  stdcall GetModuleHandleWrapW(wstr) kernel32.GetModuleHandleW
84  stdcall GetObjectWrapW(long long ptr) gdi32.GetObjectW
85  stdcall GetPrivateProfileIntWrapW(wstr wstr long wstr) kernel32.GetPrivateProfileIntW
86  stdcall GetProfileStringWrapW(wstr wstr wstr ptr long) kernel32.GetProfileStringW
87  stdcall GetPropWrapW(long wstr) user32.GetPropW
88  stdcall GetStringTypeExWrapW(long long wstr long ptr) kernel32.GetStringTypeExW
89  stdcall GetTempFileNameWrapW(wstr wstr long ptr) kernel32.GetTempFileNameW
90  stdcall GetTempPathWrapW(long ptr) kernel32.GetTempPathW
91  stdcall GetTextExtentPoint32WrapW(long wstr long ptr) gdi32.GetTextExtentPoint32W
92  stdcall GetTextFaceWrapW(long long ptr) gdi32.GetTextFaceW
93  stdcall GetTextMetricsWrapW(long ptr) gdi32.GetTextMetricsW
94  stdcall GetWindowLongWrapW(long long) user32.GetWindowLongW
95  stdcall GetWindowTextWrapW(long ptr long) user32.GetWindowTextW
96  stdcall GetWindowTextLengthWrapW(long) user32.GetWindowTextLengthW
97  stdcall GetWindowsDirectoryWrapW(ptr long) kernel32.GetWindowsDirectoryW
98  stdcall InsertMenuWrapW(long long long long ptr) user32.InsertMenuW
99  stdcall IsDialogMessageWrapW(long ptr) user32.IsDialogMessageW
100 stdcall LoadAcceleratorsWrapW(long wstr) user32.LoadAcceleratorsW
101 stdcall LoadBitmapWrapW(long wstr) user32.LoadBitmapW
102 stdcall LoadCursorWrapW(long wstr) user32.LoadCursorW
103 stdcall LoadIconWrapW(long wstr) user32.LoadIconW
104 stdcall LoadImageWrapW(long wstr long long long long) user32.LoadImageW
105 stdcall LoadLibraryExWrapW(wstr long long) kernel32.LoadLibraryExW
106 stdcall LoadMenuWrapW(long wstr) user32.LoadMenuW
107 stdcall LoadStringWrapW(long long ptr long) user32.LoadStringW
108 stdcall MessageBoxIndirectWrapW(ptr) user32.MessageBoxIndirectW
109 stdcall ModifyMenuWrapW(long long long long ptr) user32.ModifyMenuW
110 stdcall GetCharWidth32WrapW(long long long long) gdi32.GetCharWidth32W
111 stdcall GetCharacterPlacementWrapW(long wstr long long ptr long) gdi32.GetCharacterPlacementW
112 stdcall CopyFileWrapW(wstr wstr long) kernel32.CopyFileW
113 stdcall MoveFileWrapW(wstr wstr) kernel32.MoveFileW
114 stdcall OemToCharWrapW(ptr ptr) user32.OemToCharW
115 stdcall OutputDebugStringWrapW(wstr) kernel32.OutputDebugStringW
116 stdcall PeekMessageWrapW(ptr long long long long) user32.PeekMessageW
117 stdcall PostMessageWrapW(long long long long) user32.PostMessageW
118 stdcall PostThreadMessageWrapW(long long long long) user32.PostThreadMessageW
119 stdcall RegCreateKeyWrapW(long wstr ptr) advapi32.RegCreateKeyW
120 stdcall RegCreateKeyExWrapW(long wstr long ptr long long ptr ptr ptr) advapi32.RegCreateKeyExW
121 stdcall RegDeleteKeyWrapW(long wstr) advapi32.RegDeleteKeyW
122 stdcall RegEnumKeyWrapW(long long ptr long) advapi32.RegEnumKeyW
123 stdcall RegEnumKeyExWrapW(long long ptr ptr ptr ptr ptr ptr) advapi32.RegEnumKeyExW
124 stdcall RegOpenKeyWrapW(long wstr ptr) advapi32.RegOpenKeyW
125 stdcall RegOpenKeyExWrapW(long wstr long long ptr) advapi32.RegOpenKeyExW
126 stdcall RegQueryInfoKeyWrapW(long ptr ptr ptr ptr ptr ptr ptr ptr ptr ptr ptr) advapi32.RegQueryInfoKeyW
127 stdcall RegQueryValueWrapW(long wstr ptr ptr) advapi32.RegQueryValueW
128 stdcall RegQueryValueExWrapW(long wstr ptr ptr ptr ptr) advapi32.RegQueryValueExW
129 stdcall RegSetValueWrapW(long wstr long ptr long) advapi32.RegSetValueW
130 stdcall RegSetValueExWrapW(long wstr long long ptr long) advapi32.RegSetValueExW
131 stdcall RegisterClassWrapW(ptr) user32.RegisterClassW
132 stdcall RegisterClipboardFormatWrapW(wstr) user32.RegisterClipboardFormatW
133 stdcall RegisterWindowMessageWrapW(wstr) user32.RegisterWindowMessageW
134 stdcall RemovePropWrapW(long wstr) user32.RemovePropW
135 stdcall SendDlgItemMessageWrapW(long long long long long) user32.SendDlgItemMessageW
136 stdcall SendMessageWrapW(long long long long) user32.SendMessageW
137 stdcall SetCurrentDirectoryWrapW(wstr) kernel32.SetCurrentDirectoryW
138 stdcall SetDlgItemTextWrapW(long long wstr) user32.SetDlgItemTextW
139 stdcall SetMenuItemInfoWrapW(long long long ptr) user32.SetMenuItemInfoW
140 stdcall SetPropWrapW(long wstr long) user32.SetPropW
141 stdcall SetWindowLongWrapW(long long long) user32.SetWindowLongW
142 stdcall SetWindowsHookExWrapW(long long long long) user32.SetWindowsHookExW
143 stdcall SetWindowTextWrapW(long wstr) user32.SetWindowTextW
144 stdcall StartDocWrapW(long ptr) gdi32.StartDocW
145 stdcall SystemParametersInfoWrapW(long long ptr long) user32.SystemParametersInfoW
146 stdcall TranslateAcceleratorWrapW(long long ptr) user32.TranslateAcceleratorW
147 stdcall UnregisterClassWrapW(wstr long) user32.UnregisterClassW
148 stdcall VkKeyScanWrapW(long) user32.VkKeyScanW
149 stdcall WinHelpWrapW(long wstr long long) user32.WinHelpW
150 stdcall wvsprintfWrapW(ptr wstr ptr) user32.wvsprintfW
151 stdcall StrCmpNCA(str ptr long)
152 stdcall StrCmpNCW(wstr wstr long)
153 stdcall StrCmpNICA(long long long)
154 stdcall StrCmpNICW(wstr wstr long)
155 stdcall -ordinal StrCmpCA(str str)
156 stdcall -ordinal StrCmpCW(wstr wstr)
157 stdcall -ordinal StrCmpICA(str str)
158 stdcall -ordinal StrCmpICW(wstr wstr)
159 stdcall CompareStringAltW(long long wstr long wstr long) kernel32.CompareStringW
160 stdcall SHAboutInfoA(ptr long)
161 stdcall SHAboutInfoW(ptr long)
162 stdcall SHTruncateString(str long)
163 stdcall IUnknown_QueryStatus(ptr ptr long ptr ptr)
164 stdcall IUnknown_Exec(ptr ptr long long ptr ptr)
165 stdcall SHSetWindowBits(long long long long)
166 stdcall SHIsEmptyStream(ptr)
167 stdcall SHSetParentHwnd(long ptr)
168 stdcall ConnectToConnectionPoint(ptr ptr long ptr ptr ptr)
169 stdcall IUnknown_AtomicRelease(long)
170 stdcall PathSkipLeadingSlashesA(str)
171 stdcall SHIsSameObject(ptr ptr)
172 stdcall IUnknown_GetWindow(ptr ptr)
173 stdcall IUnknown_SetOwner(ptr ptr)
174 stdcall IUnknown_SetSite(ptr ptr)
175 stdcall IUnknown_GetClassID(ptr ptr)
176 stdcall IUnknown_QueryService(ptr ptr ptr ptr)
177 stdcall SHLoadMenuPopup(ptr wstr)
178 stdcall SHPropagateMessage(ptr long long long long)
179 stdcall SHMenuIndexFromID(long long)
180 stdcall SHRemoveAllSubMenus(long)
181 stdcall SHEnableMenuItem(long long long)
182 stdcall SHCheckMenuItem(long long long)
183 stdcall SHRegisterClassA(ptr)
184 stdcall IStream_Read(ptr ptr long) SHIStream_Read
185 stdcall SHMessageBoxCheckA(ptr str str long long str)
186 stdcall SHSimulateDrop(ptr ptr long ptr ptr)
187 stdcall SHLoadFromPropertyBag(ptr ptr)
188 stdcall IUnknown_TranslateAcceleratorOCS(ptr ptr long)
189 stdcall IUnknown_OnFocusOCS(ptr ptr)
190 stdcall IUnknown_HandleIRestrict(ptr ptr ptr ptr ptr)
191 stdcall SHMessageBoxCheckW(ptr wstr wstr long long wstr)
192 stdcall SHGetMenuFromID(ptr long)
193 stdcall SHGetCurColorRes()
194 stdcall SHWaitForSendMessageThread(ptr long)
195 stdcall SHIsExpandableFolder(ptr ptr)
196 stdcall SHVerbExistsNA(str ptr ptr long)
197 stdcall SHFillRectClr(long ptr long)
198 stdcall SHSearchMapInt(ptr ptr long long)
199 stdcall IUnknown_Set(ptr ptr)
200 stdcall MayQSForward(ptr ptr ptr long ptr ptr)
201 stdcall MayExecForward(ptr long ptr long long ptr ptr)
202 stdcall IsQSForward(ptr long ptr)
203 stdcall SHStripMneumonicA(str)
204 stdcall SHIsChildOrSelf(long long)
205 stdcall SHGetValueGoodBootA(long str str ptr ptr ptr)
206 stdcall SHGetValueGoodBootW(long wstr wstr ptr ptr ptr)
207 stub IContextMenu_Invoke
208 stdcall FDSA_Initialize(long long ptr ptr long)
209 stdcall FDSA_Destroy(ptr)
210 stdcall FDSA_InsertItem(ptr long ptr)
211 stdcall FDSA_DeleteItem(ptr long)
212 stdcall IStream_Write(ptr ptr long) SHIStream_Write
213 stdcall IStream_Reset(ptr)
214 stdcall IStream_Size(ptr ptr)
215 stdcall SHAnsiToUnicode(str ptr long)
216 stdcall SHAnsiToUnicodeCP(long str ptr long)
217 stdcall SHUnicodeToAnsi(wstr ptr ptr)
218 stdcall SHUnicodeToAnsiCP(long wstr ptr long)
219 stdcall QISearch(long long long long)
220 stdcall SHSetDefaultDialogFont(ptr long)
221 stdcall SHRemoveDefaultDialogFont(ptr)
222 stdcall SHGlobalCounterCreate(long)
223 stdcall SHGlobalCounterGetValue(long)
224 stdcall SHGlobalCounterIncrement(long)
225 stdcall SHStripMneumonicW(wstr)
226 stub ZoneCheckPathA
227 stub ZoneCheckPathW
228 stub ZoneCheckUrlA
229 stub ZoneCheckUrlW
230 stub ZoneCheckUrlExA
231 stdcall ZoneCheckUrlExW(wstr ptr long long long long long long)
232 stub ZoneCheckUrlExCacheA
233 stub ZoneCheckUrlExCacheW
234 stub ZoneCheckHost
235 stub ZoneCheckHostEx
236 stdcall SHPinDllOfCLSID(ptr)
237 stdcall SHRegisterClassW(ptr)
238 stdcall SHUnregisterClassesA(ptr ptr long)
239 stdcall SHUnregisterClassesW(ptr ptr long)
240 stdcall SHDefWindowProc(long long long long)
241 stdcall StopWatchMode()
242 stdcall StopWatchFlush()
243 stdcall StopWatchA(long str long long long)
244 stdcall StopWatchW(long wstr long long long)
245 stdcall StopWatch_TimerHandler(ptr ptr long ptr)
246 stub StopWatch_CheckMsg
247 stdcall StopWatch_MarkFrameStart(str)
248 stub StopWatch_MarkSameFrameStart
249 stdcall StopWatch_MarkJavaStop(wstr ptr long)
250 stdcall GetPerfTime()
251 stub StopWatch_DispatchTime
252 stdcall StopWatch_SetMsgLastLocation(long)
253 stub StopWatchExA
254 stub StopWatchExW
255 stub EventTraceHandler
256 stdcall IUnknown_GetSite(ptr ptr ptr)
257 stdcall SHCreateWorkerWindowA(ptr ptr long long ptr long)
258 stub SHRegisterWaitForSingleObject
259 stub SHUnregisterWait
260 stdcall SHQueueUserWorkItem(long long long long long long long)
261 stub SHCreateTimerQueue
262 stub SHDeleteTimerQueue
263 stdcall SHSetTimerQueueTimer(long ptr ptr long long str long)
264 stub SHChangeTimerQueueTimer
265 stub SHCancelTimerQueueTimer
266 stdcall SHRestrictionLookup(long wstr ptr ptr)
267 stdcall SHWeakQueryInterface(long long long long)
268 stdcall SHWeakReleaseInterface(long long)
269 stdcall GUIDFromStringA(str ptr)
270 stdcall GUIDFromStringW(wstr ptr)
271 stdcall SHGetRestriction(wstr wstr wstr)
272 stub SHSetThreadPoolLimits
273 stub SHTerminateThreadPool
274 stub RegisterGlobalHotkeyW
275 stub RegisterGlobalHotkeyA
276 stdcall WhichPlatform()
277 stub SHDialogBox
278 stdcall SHCreateWorkerWindowW(ptr ptr long long ptr long)
279 stdcall SHInvokeDefaultCommand(ptr ptr ptr)
280 stdcall SHRegGetIntW(ptr wstr long)
281 stdcall SHPackDispParamsV(ptr ptr long ptr)
282 varargs SHPackDispParams(ptr ptr long)
283 stdcall IConnectionPoint_InvokeWithCancel(ptr long long long long)
284 stdcall IConnectionPoint_SimpleInvoke(ptr long ptr)
285 stdcall IConnectionPoint_OnChanged(ptr long)
286 varargs IUnknown_CPContainerInvokeParam(ptr ptr long ptr long)
287 stdcall IUnknown_CPContainerOnChanged(ptr long)
288 stub IUnknown_CPContainerInvokeIndirect
289 stdcall PlaySoundWrapW(wstr long long)
290 stub SHMirrorIcon
291 stdcall SHMessageBoxCheckExA(ptr ptr ptr ptr ptr long str)
292 stdcall SHMessageBoxCheckExW(ptr ptr ptr ptr ptr long wstr)
293 stub SHCancelUserWorkItems
294 stdcall SHGetIniStringW(wstr wstr ptr long wstr)
295 stdcall SHSetIniStringW(wstr ptr wstr wstr)
296 stub CreateURLFileContentsW
297 stub CreateURLFileContentsA
298 stdcall WritePrivateProfileStringWrapW(wstr wstr wstr wstr) kernel32.WritePrivateProfileStringW
299 stdcall ExtTextOutWrapW(long long long long ptr wstr long ptr) gdi32.ExtTextOutW
300 stdcall CreateFontWrapW(long long long long long long long long long long long long long wstr) gdi32.CreateFontW
301 stdcall DrawTextExWrapW(long wstr long ptr long ptr) user32.DrawTextExW
302 stdcall GetMenuItemInfoWrapW(long long long ptr) user32.GetMenuItemInfoW
303 stdcall InsertMenuItemWrapW(long long long ptr) user32.InsertMenuItemW
304 stdcall CreateMetaFileWrapW(wstr) gdi32.CreateMetaFileW
305 stdcall CreateMutexWrapW(ptr long wstr) kernel32.CreateMutexW
306 stdcall ExpandEnvironmentStringsWrapW(wstr ptr long) kernel32.ExpandEnvironmentStringsW
307 stdcall CreateSemaphoreWrapW(ptr long long wstr) kernel32.CreateSemaphoreW
308 stdcall IsBadStringPtrWrapW(ptr long) kernel32.IsBadStringPtrW
309 stdcall LoadLibraryWrapW(wstr) kernel32.LoadLibraryW
310 stdcall GetTimeFormatWrapW(long long ptr wstr ptr long) kernel32.GetTimeFormatW
311 stdcall GetDateFormatWrapW(long long ptr wstr ptr long) kernel32.GetDateFormatW
312 stdcall GetPrivateProfileStringWrapW(wstr wstr wstr ptr long wstr) kernel32.GetPrivateProfileStringW
313 stdcall SHGetFileInfoWrapW(ptr long ptr long long)
314 stdcall RegisterClassExWrapW(ptr) user32.RegisterClassExW
315 stdcall GetClassInfoExWrapW(long wstr ptr) user32.GetClassInfoExW
316 stub IShellFolder_GetDisplayNameOf
317 stub IShellFolder_ParseDisplayName
318 stdcall DragQueryFileWrapW(long long wstr long)
319 stdcall FindWindowExWrapW(long long wstr wstr) user32.FindWindowExW
320 stdcall RegisterMIMETypeForExtensionA(str str)
321 stdcall RegisterMIMETypeForExtensionW(wstr wstr)
322 stdcall UnregisterMIMETypeForExtensionA(str)
323 stdcall UnregisterMIMETypeForExtensionW(wstr)
324 stdcall RegisterExtensionForMIMETypeA(str str)
325 stdcall RegisterExtensionForMIMETypeW(wstr wstr)
326 stdcall UnregisterExtensionForMIMETypeA(str)
327 stdcall UnregisterExtensionForMIMETypeW(wstr)
328 stdcall GetMIMETypeSubKeyA(str ptr long)
329 stdcall GetMIMETypeSubKeyW(wstr ptr long)
330 stdcall MIME_GetExtensionA(str ptr long)
331 stdcall MIME_GetExtensionW(wstr ptr long)
332 stdcall CallMsgFilterWrapW(ptr long) user32.CallMsgFilterW
333 stdcall SHBrowseForFolderWrapW(ptr)
334 stdcall SHGetPathFromIDListWrapW(ptr ptr)
335 stdcall ShellExecuteExWrapW(ptr)
336 stdcall SHFileOperationWrapW(ptr)
337 stdcall ExtractIconExWrapW(wstr long ptr ptr long) user32.PrivateExtractIconExW
338 stdcall SetFileAttributesWrapW(wstr long) kernel32.SetFileAttributesW
339 stdcall GetNumberFormatWrapW(long long wstr ptr ptr long) kernel32.GetNumberFormatW
340 stdcall MessageBoxWrapW(long wstr wstr long) user32.MessageBoxW
341 stdcall FindNextFileWrapW(long ptr) kernel32.FindNextFileW
342 stdcall SHInterlockedCompareExchange(ptr ptr ptr)
343 stdcall SHRegGetCLSIDKeyA(ptr str long long ptr)
344 stdcall SHRegGetCLSIDKeyW(ptr wstr long long ptr)
345 stdcall SHAnsiToAnsi(str ptr long)
346 stdcall SHUnicodeToUnicode(wstr ptr long)
347 stdcall RegDeleteValueWrapW(long wstr) advapi32.RegDeleteValueW
348 stub SHGetFileDescriptionW
349 stub SHGetFileDescriptionA
350 stdcall GetFileVersionInfoSizeWrapW(wstr ptr)
351 stdcall GetFileVersionInfoWrapW(wstr long long ptr)
352 stdcall VerQueryValueWrapW(ptr wstr ptr ptr)
353 stdcall SHFormatDateTimeA(ptr ptr str long)
354 stdcall SHFormatDateTimeW(ptr ptr wstr long)
355 stdcall IUnknown_EnableModeless(ptr long)
356 stdcall AssocCreate(int128 ptr ptr)
357 stdcall SHGetNewLinkInfoWrapW(wstr wstr wstr long long)
358 stdcall SHDefExtractIconWrapW(wstr long long ptr ptr long)
359 stdcall OpenEventWrapW(long long wstr) kernel32.OpenEventW
360 stdcall RemoveDirectoryWrapW(wstr) kernel32.RemoveDirectoryW
361 stdcall GetShortPathNameWrapW(wstr ptr long) kernel32.GetShortPathNameW
362 stdcall GetUserNameWrapW(ptr ptr) advapi32.GetUserNameW
363 stdcall SHInvokeCommand(ptr ptr ptr long)
364 stdcall DoesStringRoundTripA(str ptr long)
365 stdcall DoesStringRoundTripW(wstr ptr long)
366 stdcall RegEnumValueWrapW(long long ptr ptr ptr ptr ptr ptr) advapi32.RegEnumValueW
367 stdcall WritePrivateProfileStructWrapW(wstr wstr ptr long wstr) kernel32.WritePrivateProfileStructW
368 stdcall GetPrivateProfileStructWrapW(wstr wstr ptr long wstr) kernel32.GetPrivateProfileStructW
369 stdcall CreateProcessWrapW(wstr wstr ptr ptr long long ptr wstr ptr ptr) kernel32.CreateProcessW
370 stdcall ExtractIconWrapW(long wstr long)
371 stdcall DdeInitializeWrapW(ptr ptr long long) user32.DdeInitializeW
372 stdcall DdeCreateStringHandleWrapW(long ptr long) user32.DdeCreateStringHandleW
373 stdcall DdeQueryStringWrapW(long ptr wstr long long) user32.DdeQueryStringW
374 stub SHCheckDiskForMediaA
375 stub SHCheckDiskForMediaW
376 stdcall MLGetUILanguage() kernel32.GetUserDefaultUILanguage
377 stdcall MLLoadLibraryA(str long long)
378 stdcall MLLoadLibraryW(wstr long long)
379 stub Shell_GetCachedImageIndexWrapW
380 stub Shell_GetCachedImageIndexWrapA
381 stub AssocCopyVerbs
382 stdcall ZoneComputePaneSize(ptr)
383 stub ZoneConfigureW
384 stub SHRestrictedMessageBox
385 stub SHLoadRawAccelerators
386 stub SHQueryRawAccelerator
387 stub SHQueryRawAcceleratorMsg
388 varargs ShellMessageBoxWrapW(ptr ptr wstr wstr long)
389 stdcall GetSaveFileNameWrapW(ptr)
390 stdcall WNetRestoreConnectionWrapW(long wstr)
391 stdcall WNetGetLastErrorWrapW(ptr ptr long ptr long)
392 stdcall EndDialogWrap(ptr ptr) user32.EndDialog
393 stdcall CreateDialogIndirectParamWrapW(long ptr long ptr long) user32.CreateDialogIndirectParamW
394 stdcall SHChangeNotifyWrap(long long ptr ptr)
395 stub MLWinHelpA
396 stub MLHtmlHelpA
397 stub MLWinHelpW
398 stub MLHtmlHelpW
399 stdcall StrCpyNXA(ptr str long)
400 stdcall StrCpyNXW(ptr wstr long)
401 stdcall PageSetupDlgWrapW(ptr)
402 stdcall PrintDlgWrapW(ptr)
403 stdcall GetOpenFileNameWrapW(ptr)
404 stdcall IShellFolder_EnumObjects(ptr ptr long ptr) SHIShellFolder_EnumObjects
405 stdcall MLBuildResURLA(str ptr long str ptr long)
406 stdcall MLBuildResURLW(wstr ptr long wstr ptr long)
407 stub AssocMakeProgid
408 stub AssocMakeShell
409 stub AssocMakeApplicationByKeyW
410 stub AssocMakeApplicationByKeyA
411 stub AssocMakeFileExtsToApplicationW
412 stub AssocMakeFileExtsToApplicationA
413 stdcall SHGetMachineInfo(long)
414 stub SHHtmlHelpOnDemandW
415 stub SHHtmlHelpOnDemandA
416 stdcall SHWinHelpOnDemandW(long wstr long ptr long)
417 stdcall SHWinHelpOnDemandA(long str long ptr long)
418 stdcall MLFreeLibrary(long)
419 stdcall SHFlushSFCacheWrap()
420 stub SHLWAPI_420 # CMemStream::Commit ??
421 stub SHLoadPersistedDataObject
422 stdcall SHGlobalCounterCreateNamedA(str long)
423 stdcall SHGlobalCounterCreateNamedW(wstr long)
424 stdcall SHGlobalCounterDecrement(long)
425 stdcall DeleteMenuWrap(ptr long long) user32.DeleteMenu
426 stdcall DestroyMenuWrap(long) user32.DestroyMenu
427 stdcall TrackPopupMenuWrap(long long long long long long ptr) user32.TrackPopupMenu
428 stdcall TrackPopupMenuExWrap(long long long long long ptr) user32.TrackPopupMenuEx
429 stdcall MLIsMLHInstance(long)
430 stdcall MLSetMLHInstance(long long)
431 stdcall MLClearMLHInstance(long)
432 stdcall SHSendMessageBroadcastA(long long long)
433 stdcall SHSendMessageBroadcastW(long long long)
434 stdcall SendMessageTimeoutWrapW(long long long long long long ptr) user32.SendMessageTimeoutW
435 stdcall CLSIDFromProgIDWrap(wstr ptr) ole32.CLSIDFromProgID
436 stdcall CLSIDFromStringWrap(wstr ptr)
437 stdcall IsOS(long)
438 stub SHLoadRegUIStringA
439 stdcall SHLoadRegUIStringW(ptr wstr ptr long)
440 stdcall SHGetWebFolderFilePathA(str ptr long)
441 stdcall SHGetWebFolderFilePathW(wstr ptr long)
442 stdcall GetEnvironmentVariableWrapW(wstr ptr long) kernel32.GetEnvironmentVariableW
443 stdcall SHGetSystemWindowsDirectoryA(ptr long) kernel32.GetSystemWindowsDirectoryA
444 stdcall SHGetSystemWindowsDirectoryW(ptr long) kernel32.GetSystemWindowsDirectoryW
445 stdcall PathFileExistsAndAttributesA(str ptr)
446 stdcall PathFileExistsAndAttributesW(wstr ptr)
447 stub FixSlashesAndColonA
448 stdcall FixSlashesAndColonW(wstr)
449 stub NextPathA
450 stub NextPathW
451 stub CharUpperNoDBCSA
452 stub CharUpperNoDBCSW
453 stub CharLowerNoDBCSA
454 stub CharLowerNoDBCSW
455 stdcall PathIsValidCharA(long long)
456 stdcall PathIsValidCharW(long long)
457 stdcall GetLongPathNameWrapW(wstr ptr long) kernel32.GetLongPathNameW
458 stdcall GetLongPathNameWrapA(str ptr long) kernel32.GetLongPathNameA
459 stdcall SHExpandEnvironmentStringsA(str ptr long) kernel32.ExpandEnvironmentStringsA
460 stdcall SHExpandEnvironmentStringsW(wstr ptr long) kernel32.ExpandEnvironmentStringsW
461 stdcall SHGetAppCompatFlags(long)
462 stdcall UrlFixupW(wstr wstr long)
463 stdcall SHExpandEnvironmentStringsForUserA(ptr str ptr long) userenv.ExpandEnvironmentStringsForUserA
464 stdcall SHExpandEnvironmentStringsForUserW(ptr wstr ptr long) userenv.ExpandEnvironmentStringsForUserW
465 stub PathUnExpandEnvStringsForUserA
466 stdcall -stub PathUnExpandEnvStringsForUserW(ptr wstr ptr long)
467 stub -ordinal SHRunIndirectRegClientCommand
468 stub RunIndirectRegCommand
469 stub RunRegCommand
470 stub IUnknown_ProfferServiceOld
471 stdcall SHCreatePropertyBagOnRegKey(long wstr long ptr ptr)
472 stub SHCreatePropertyBagOnProfileSelection
473 stub SHGetIniStringUTF7W
474 stub SHSetIniStringUTF7W
475 stdcall GetShellSecurityDescriptor(ptr long)
476 stdcall SHGetObjectCompatFlags(ptr ptr)
477 stub SHCreatePropertyBagOnMemory
478 stdcall IUnknown_TranslateAcceleratorIO(ptr ptr)
479 stdcall IUnknown_UIActivateIO(ptr long ptr)
480 stdcall UrlCrackW(wstr long long ptr) wininet.InternetCrackUrlW
481 stdcall IUnknown_HasFocusIO(ptr)
482 stub SHMessageBoxHelpA
483 stub SHMessageBoxHelpW
484 stdcall IUnknown_QueryServiceExec(ptr ptr ptr long long long ptr)
485 stub MapWin32ErrorToSTG
486 stub ModeToCreateFileFlags
487 stdcall -ordinal SHLoadIndirectString(wstr ptr long ptr)
488 stub SHConvertGraphicsFile
489 stdcall GlobalAddAtomWrapW(wstr) kernel32.GlobalAddAtomW
490 stdcall GlobalFindAtomWrapW(wstr) kernel32.GlobalFindAtomW
491 stdcall SHGetShellKey(long long long)
492 stub PrettifyFileDescriptionW
493 stub SHPropertyBag_ReadType
494 stub SHPropertyBag_ReadStr
495 stdcall SHPropertyBag_WriteStr(ptr wstr wstr)
496 stdcall SHPropertyBag_ReadLONG(ptr wstr ptr)
497 stdcall SHPropertyBag_WriteLONG(ptr wstr long)
498 stub SHPropertyBag_ReadBOOLOld
499 stub SHPropertyBag_WriteBOOL
500 stdcall AssocGetPerceivedType(wstr ptr ptr ptr)
501 stdcall AssocIsDangerous(wstr)
502 stdcall AssocQueryKeyA(long long str str ptr)
503 stdcall AssocQueryKeyW(long long wstr wstr ptr)
504 stdcall AssocQueryStringA(long long str str ptr ptr)
505 stub SHPropertyBag_ReadGUID
506 stub SHPropertyBag_WriteGUID
507 stdcall -stub SHPropertyBag_ReadDWORD(ptr ptr ptr)
508 stub SHPropertyBag_WriteDWORD
509 stdcall IUnknown_OnFocusChangeIS(ptr ptr long)
510 stdcall SHLockSharedEx(ptr long long)
511 stdcall -stub PathFileExistsDefExtAndAttributesW(wstr long ptr)
512 stub -ordinal IStream_ReadPidl
513 stub -ordinal IStream_WritePidl
514 stdcall IUnknown_ProfferService(ptr ptr ptr ptr)
515 stdcall -ordinal SHGetViewStatePropertyBag(ptr wstr long ptr ptr)
516 stdcall SKGetValueW(long wstr wstr ptr ptr ptr)
517 stdcall SKSetValueW(long wstr wstr long ptr long)
518 stdcall SKDeleteValueW(long wstr wstr)
519 stdcall SKAllocValueW(long wstr wstr ptr ptr ptr)
520 stub SHPropertyBag_ReadBSTR
521 stub SHPropertyBag_ReadPOINTL
522 stub SHPropertyBag_WritePOINTL
523 stub SHPropertyBag_ReadRECTL
524 stub SHPropertyBag_WriteRECTL
525 stub SHPropertyBag_ReadPOINTS
526 stub SHPropertyBag_WritePOINTS
527 stub SHPropertyBag_ReadSHORT
528 stub SHPropertyBag_WriteSHORT
529 stub SHPropertyBag_ReadInt
530 stub SHPropertyBag_WriteInt
531 stub SHPropertyBag_ReadStream
532 stub SHPropertyBag_WriteStream
533 stub SHGetPerScreenResName
534 stub SHPropertyBag_ReadBOOL
535 stub SHPropertyBag_Delete
536 stdcall -stub IUnknown_QueryServicePropertyBag(ptr long ptr ptr)
537 stub SHBoolSystemParametersInfo
538 stdcall IUnknown_QueryServiceForWebBrowserApp(ptr ptr ptr)
539 stub IUnknown_ShowBrowserBar
540 stub SHInvokeCommandOnContextMenu
541 stub SHInvokeCommandsOnContextMen
542 stdcall GetUIVersion()
543 stdcall CreateColorSpaceWrapW(ptr) gdi32.CreateColorSpaceW
544 stub QuerySourceCreateFromKey
545 stdcall SHForwardContextMenuMsg(ptr long long long ptr long)
546 stub IUnknown_DoContextMenuPopup
547 stdcall DelayLoadFailureHook(str str) kernel32.DelayLoadFailureHook
548 stdcall SHAreIconsEqual(ptr ptr)
549 stdcall SHCoCreateInstanceAC(ptr ptr long ptr ptr)
550 stub GetTemplateInfoFromHandle
551 stub IShellFolder_CompareIDs
552 stdcall -stub -version=0x501-0x502 SHEvaluateSystemCommandTemplate(wstr ptr ptr ptr)
553 stdcall IsInternetESCEnabled()
554 stdcall -stub SHGetAllAccessSA()
555 stdcall AssocQueryStringByKeyA(long long ptr str ptr ptr)
556 stub SHCoExtensionAllowed
557 stub SHCoCreateExtension
558 stub SHCoExtensionCollectStats
559 stub SHGetSignatureInfo
560 stdcall -stub SHWindowsPolicyGetValue(ptr ptr ptr)
561 stub AssocGetUrlAction
562 stub SHGetPrivateProfileInt
563 stdcall -stub SHGetPrivateProfileSection(wstr ptr long ptr)
564 stub SHGetPrivateProfileSectionNames
565 stub SHGetPrivateProfileString
566 stub SHGetPrivateProfileStruct
567 stdcall AssocQueryStringByKeyW(long long ptr wstr ptr ptr)
568 stdcall AssocQueryStringW(long long wstr wstr ptr ptr)
569 stdcall ChrCmpIA(long long)
570 stdcall ChrCmpIW(long long)
571 stdcall ColorAdjustLuma(long long long)
572 stdcall ColorHLSToRGB(long long long)
573 stdcall ColorRGBToHLS(long ptr ptr ptr)
@ stdcall -private DllGetVersion(ptr)
575 stdcall GetMenuPosFromID(ptr long)
576 stdcall HashData(ptr long ptr long)
577 stdcall IntlStrEqWorkerA(long str str long) StrIsIntlEqualA
578 stdcall IntlStrEqWorkerW(long wstr wstr long) StrIsIntlEqualW
579 stdcall IsCharSpaceA(long)
580 stdcall PathAddBackslashA(str)
581 stdcall PathAddBackslashW(wstr)
582 stdcall PathAddExtensionA(str str)
583 stdcall PathAddExtensionW(wstr wstr)
584 stdcall PathAppendA(str str)
585 stdcall PathAppendW(wstr wstr)
586 stdcall PathBuildRootA(ptr long)
587 stdcall PathBuildRootW(ptr long)
588 stdcall PathCanonicalizeA(ptr str)
589 stdcall PathCanonicalizeW(ptr wstr)
590 stdcall PathCombineA(ptr str str)
591 stdcall PathCombineW(ptr wstr wstr)
592 stdcall PathCommonPrefixA(str str ptr)
593 stdcall PathCommonPrefixW(wstr wstr ptr)
594 stdcall PathCompactPathA(long str long)
595 stdcall PathCompactPathExA(ptr str long long)
596 stdcall PathCompactPathExW(ptr wstr long long)
597 stdcall PathCompactPathW(long wstr long)
598 stdcall PathCreateFromUrlA(str ptr ptr long)
@   stdcall -version=0x600+ PathCreateFromUrlAlloc(wstr wstr long)
599 stdcall PathCreateFromUrlW(wstr ptr ptr long)
600 stdcall PathFileExistsA(str)
601 stdcall PathFileExistsW(wstr)
602 stdcall PathFindExtensionA(str)
603 stdcall PathFindExtensionW(wstr)
604 stdcall PathFindFileNameA(str)
605 stdcall PathFindFileNameW(wstr)
606 stdcall PathFindNextComponentA(str)
607 stdcall PathFindNextComponentW(wstr)
608 stdcall PathFindOnPathA(str ptr)
609 stdcall PathFindOnPathW(wstr ptr)
610 stdcall PathFindSuffixArrayA(str ptr long)
611 stdcall PathFindSuffixArrayW(wstr ptr long)
612 stdcall PathGetArgsA(str)
613 stdcall PathGetArgsW(wstr)
614 stdcall PathGetCharTypeA(long)
615 stdcall PathGetCharTypeW(long)
616 stdcall PathGetDriveNumberA(str)
617 stdcall PathGetDriveNumberW(wstr)
618 stdcall PathIsContentTypeA(str str)
619 stdcall PathIsContentTypeW(wstr wstr)
620 stdcall PathIsDirectoryA(str)
621 stdcall PathIsDirectoryEmptyA(str)
622 stdcall PathIsDirectoryEmptyW(wstr)
623 stdcall PathIsDirectoryW(wstr)
624 stdcall PathIsFileSpecA(str)
625 stdcall PathIsFileSpecW(wstr)
626 stdcall PathIsLFNFileSpecA(str)
627 stdcall PathIsLFNFileSpecW(wstr)
628 stdcall PathIsNetworkPathA(str)
629 stdcall PathIsNetworkPathW(wstr)
630 stdcall PathIsPrefixA(str str)
631 stdcall PathIsPrefixW(wstr wstr)
632 stdcall PathIsRelativeA(str)
633 stdcall PathIsRelativeW(wstr)
634 stdcall PathIsRootA(str)
635 stdcall PathIsRootW(wstr)
636 stdcall PathIsSameRootA(str str)
637 stdcall PathIsSameRootW(wstr wstr)
638 stdcall PathIsSystemFolderA(str long)
639 stdcall PathIsSystemFolderW(wstr long)
640 stdcall PathIsUNCA(str)
641 stdcall PathIsUNCServerA(str)
642 stdcall PathIsUNCServerShareA(str)
643 stdcall PathIsUNCServerShareW(wstr)
644 stdcall PathIsUNCServerW(wstr)
645 stdcall PathIsUNCW(wstr)
646 stdcall PathIsURLA(str)
647 stdcall PathIsURLW(wstr)
648 stdcall PathMakePrettyA(str)
649 stdcall PathMakePrettyW(wstr)
650 stdcall PathMakeSystemFolderA(str)
651 stdcall PathMakeSystemFolderW(wstr)
652 stdcall PathMatchSpecA(str str)
@   stdcall -stub -version=0x600+ PathMatchSpecExA(str wstr long)
@   stdcall -stub -version=0x600+ PathMatchSpecExW(wstr wstr long)
653 stdcall PathMatchSpecW(wstr wstr)
654 stdcall PathParseIconLocationA(str)
655 stdcall PathParseIconLocationW(wstr)
656 stdcall PathQuoteSpacesA(str)
657 stdcall PathQuoteSpacesW(wstr)
658 stdcall PathRelativePathToA(ptr str long str long)
659 stdcall PathRelativePathToW(ptr wstr long wstr long)
660 stdcall PathRemoveArgsA(str)
661 stdcall PathRemoveArgsW(wstr)
662 stdcall PathRemoveBackslashA(str)
663 stdcall PathRemoveBackslashW(wstr)
664 stdcall PathRemoveBlanksA(str)
665 stdcall PathRemoveBlanksW(wstr)
666 stdcall PathRemoveExtensionA(str)
667 stdcall PathRemoveExtensionW(wstr)
668 stdcall PathRemoveFileSpecA(str)
669 stdcall PathRemoveFileSpecW(wstr)
670 stdcall PathRenameExtensionA(str str)
671 stdcall PathRenameExtensionW(wstr wstr)
672 stdcall PathSearchAndQualifyA(str ptr long)
673 stdcall PathSearchAndQualifyW(wstr ptr long)
674 stdcall PathSetDlgItemPathA(long long ptr)
675 stdcall PathSetDlgItemPathW(long long ptr)
676 stdcall PathSkipRootA(str)
677 stdcall PathSkipRootW(wstr)
678 stdcall PathStripPathA(str)
679 stdcall PathStripPathW(wstr)
680 stdcall PathStripToRootA(str)
681 stdcall PathStripToRootW(wstr)
682 stdcall PathUnExpandEnvStringsA(str ptr long)
683 stdcall PathUnExpandEnvStringsW(wstr ptr long)
684 stdcall PathUndecorateA(str)
685 stdcall PathUndecorateW(wstr)
686 stdcall PathUnmakeSystemFolderA(str)
687 stdcall PathUnmakeSystemFolderW(wstr)
688 stdcall PathUnquoteSpacesA(str)
689 stdcall PathUnquoteSpacesW(wstr)
690 stdcall SHAutoComplete(ptr long)
691 stdcall SHCopyKeyA(long str long long)
692 stdcall SHCopyKeyW(long wstr long long)
693 stdcall SHCreateShellPalette(long)
694 stdcall SHCreateStreamOnFileA(str long ptr)
695 stdcall SHCreateStreamOnFileEx(wstr long long long ptr ptr)
696 stdcall SHCreateStreamOnFileW(wstr long ptr)
697 stdcall SHCreateStreamWrapper(ptr ptr long ptr)
698 stdcall SHCreateThreadRef(ptr ptr)
699 stdcall SHDeleteEmptyKeyA(long ptr)
700 stdcall SHDeleteEmptyKeyW(long ptr)
701 stdcall SHDeleteKeyA(long str)
702 stdcall SHDeleteKeyW(long wstr)
703 stdcall SHDeleteOrphanKeyA(long str)
704 stdcall SHDeleteOrphanKeyW(long wstr)
705 stdcall SHDeleteValueA(long str str)
706 stdcall SHDeleteValueW(long wstr wstr)
707 stdcall SHEnumKeyExA(long long str ptr)
708 stdcall SHEnumKeyExW(long long wstr ptr)
709 stdcall SHEnumValueA(long long str ptr ptr ptr ptr)
710 stdcall SHEnumValueW(long long wstr ptr ptr ptr ptr)
711 stdcall SHGetInverseCMAP(ptr long)
712 stdcall SHGetThreadRef(ptr)
713 stdcall SHGetValueA(long str str ptr ptr ptr)
714 stdcall SHGetValueW(long wstr wstr ptr ptr ptr)
715 stdcall SHIsLowMemoryMachine(long)
716 stdcall SHOpenRegStream2A(long str str long)
717 stdcall SHOpenRegStream2W(long wstr wstr long)
718 stdcall SHOpenRegStreamA(long str str long)
719 stdcall SHOpenRegStreamW(long wstr wstr long)
720 stdcall SHQueryInfoKeyA(long ptr ptr ptr ptr)
721 stdcall SHQueryInfoKeyW(long ptr ptr ptr ptr)
722 stdcall SHQueryValueExA(long str ptr ptr ptr ptr)
723 stdcall SHQueryValueExW(long wstr ptr ptr ptr ptr)
724 stdcall SHRegCloseUSKey(ptr)
725 stdcall SHRegCreateUSKeyA(str long long ptr long)
726 stdcall SHRegCreateUSKeyW(wstr long long ptr long)
727 stdcall SHRegDeleteEmptyUSKeyA(long str long)
728 stdcall SHRegDeleteEmptyUSKeyW(long wstr long)
729 stdcall SHRegDeleteUSValueA(long str long)
730 stdcall SHRegDeleteUSValueW(long wstr long)
731 stdcall SHRegDuplicateHKey(long)
732 stdcall SHRegEnumUSKeyA(long long str ptr long)
733 stdcall SHRegEnumUSKeyW(long long wstr ptr long)
734 stdcall SHRegEnumUSValueA(long long ptr ptr ptr ptr ptr long)
735 stdcall SHRegEnumUSValueW(long long ptr ptr ptr ptr ptr long)
736 stdcall SHRegGetBoolUSValueA(str str long long)
737 stdcall SHRegGetBoolUSValueW(wstr wstr long long)
738 stdcall SHRegGetPathA(long str str ptr long)
739 stdcall SHRegGetPathW(long wstr wstr ptr long)
740 stdcall SHRegGetUSValueA(str str ptr ptr ptr long ptr long)
741 stdcall SHRegGetUSValueW(wstr wstr ptr ptr ptr long ptr long)
742 stdcall SHRegGetValueA(long str str long ptr ptr ptr) advapi32.RegGetValueA
743 stdcall SHRegGetValueW(long wstr wstr long ptr ptr ptr) advapi32.RegGetValueW
744 stdcall SHRegOpenUSKeyA(str long long long long)
745 stdcall SHRegOpenUSKeyW(wstr long long long long)
746 stdcall SHRegQueryInfoUSKeyA(long ptr ptr ptr ptr long)
747 stdcall SHRegQueryInfoUSKeyW(long ptr ptr ptr ptr long)
748 stdcall SHRegQueryUSValueA(long str ptr ptr ptr long ptr long)
749 stdcall SHRegQueryUSValueW(long wstr ptr ptr ptr long ptr long)
750 stdcall SHRegSetPathA(long str str str long)
751 stdcall SHRegSetPathW(long wstr wstr wstr long)
752 stdcall SHRegSetUSValueA(str str long ptr long long)
753 stdcall SHRegSetUSValueW(wstr wstr long ptr long long)
754 stdcall SHRegWriteUSValueA(long str long ptr long long)
755 stdcall SHRegWriteUSValueW(long wstr long ptr long long)
756 stdcall SHRegisterValidateTemplate(wstr long)
757 stdcall SHReleaseThreadRef()
758 stdcall SHSetThreadRef(ptr)
759 stdcall SHSetValueA(long str str long ptr long)
760 stdcall SHSetValueW(long wstr wstr long ptr long)
761 stdcall SHSkipJunction(ptr ptr)
762 stdcall SHStrDupA(str ptr)
763 stdcall SHStrDupW(wstr ptr)
764 stdcall StrCSpnA(str str)
765 stdcall StrCSpnIA(str str)
766 stdcall StrCSpnIW(wstr wstr)
767 stdcall StrCSpnW(wstr wstr)
768 stdcall StrCatBuffA(str str long)
769 stdcall StrCatBuffW(wstr wstr long)
770 stdcall StrCatChainW (ptr long long wstr)
771 stdcall StrCatW(ptr wstr)
772 stdcall StrChrA(str long)
773 stdcall StrChrIA(str long)
774 stdcall StrChrIW(wstr long)
775 stub StrChrNIW
776 stdcall StrChrNW(wstr long long)
777 stdcall StrChrW(wstr long)
778 stdcall StrCmpIW(wstr wstr)
779 stdcall StrCmpLogicalW(wstr wstr)
780 stdcall StrCmpNA(str str long)
781 stdcall StrCmpNIA(str str long)
782 stdcall StrCmpNIW(wstr wstr long)
783 stdcall StrCmpNW(wstr wstr long)
784 stdcall StrCmpW(wstr wstr)
785 stdcall StrCpyNW(ptr wstr long)
786 stdcall StrCpyW(ptr wstr)
787 stdcall StrDupA(str)
788 stdcall StrDupW(wstr)
789 stdcall StrFormatByteSize64A(int64 ptr long)
790 stdcall StrFormatByteSizeA(long ptr long)
@   stdcall -stub -version=0x600+ StrFormatByteSizeEx(int64 ptr wstr long)
791 stdcall StrFormatByteSizeW(int64 ptr long)
792 stdcall StrFormatKBSizeA(int64 str long)
793 stdcall StrFormatKBSizeW(int64 wstr long)
794 stdcall StrFromTimeIntervalA(ptr long long long)
795 stdcall StrFromTimeIntervalW(ptr long long long)
796 stdcall StrIsIntlEqualA(long str str long)
797 stdcall StrIsIntlEqualW(long wstr wstr long)
798 stdcall StrNCatA(str str long)
799 stdcall StrNCatW(wstr wstr long)
800 stdcall StrPBrkA(str str)
801 stdcall StrPBrkW(wstr wstr)
802 stdcall StrRChrA(str str long)
803 stdcall StrRChrIA(str str long)
804 stdcall StrRChrIW(wstr wstr long)
805 stdcall StrRChrW(wstr wstr long)
806 stdcall StrRStrIA(str str str)
807 stdcall StrRStrIW(wstr wstr wstr)
808 stdcall StrRetToBSTR(ptr ptr ptr)
809 stdcall StrRetToBufA(ptr ptr ptr long)
810 stdcall StrRetToBufW(ptr ptr ptr long)
811 stdcall StrRetToStrA(ptr ptr ptr)
812 stdcall StrRetToStrW(ptr ptr ptr)
813 stdcall StrSpnA(str str)
814 stdcall StrSpnW(wstr wstr)
815 stdcall StrStrA(str str)
816 stdcall StrStrIA(str str)
817 stdcall StrStrIW(wstr wstr)
818 stdcall StrStrNIW(wstr wstr long)
819 stdcall StrStrNW(wstr wstr long)
820 stdcall StrStrW(wstr wstr)
821 stdcall StrToInt64ExA(str long ptr)
822 stdcall StrToInt64ExW(wstr long ptr)
823 stdcall StrToIntA(str)
824 stdcall StrToIntExA(str long ptr)
825 stdcall StrToIntExW(wstr long ptr)
826 stdcall StrToIntW(wstr)
827 stdcall StrTrimA(str str)
828 stdcall StrTrimW(wstr wstr)
829 stdcall UrlApplySchemeA(str ptr ptr long)
830 stdcall UrlApplySchemeW(wstr ptr ptr long)
831 stdcall UrlCanonicalizeA(str ptr ptr long)
832 stdcall UrlCanonicalizeW(wstr ptr ptr long)
833 stdcall UrlCombineA(str str ptr ptr long)
834 stdcall UrlCombineW(wstr wstr ptr ptr long)
835 stdcall UrlCompareA(str str long)
836 stdcall UrlCompareW(wstr wstr long)
837 stdcall UrlCreateFromPathA(str ptr ptr long)
838 stdcall UrlCreateFromPathW(wstr ptr ptr long)
839 stdcall UrlEscapeA(str ptr ptr long)
840 stdcall UrlEscapeW(wstr ptr ptr long)
841 stdcall UrlGetLocationA(str)
842 stdcall UrlGetLocationW(wstr)
843 stdcall UrlGetPartA(str ptr ptr long long)
844 stdcall UrlGetPartW(wstr ptr ptr long long)
845 stdcall UrlHashA(str ptr long)
846 stdcall UrlHashW(wstr ptr long)
847 stdcall UrlIsA(str long)
848 stdcall UrlIsNoHistoryA(str)
849 stdcall UrlIsNoHistoryW(wstr)
850 stdcall UrlIsOpaqueA(str)
851 stdcall UrlIsOpaqueW(wstr)
852 stdcall UrlIsW(wstr long)
853 stdcall UrlUnescapeA(str ptr ptr long)
854 stdcall UrlUnescapeW(wstr ptr ptr long)
855 varargs wnsprintfA(ptr long str)
856 varargs wnsprintfW(ptr long wstr)
857 stdcall wvnsprintfA(ptr long str ptr)
858 stdcall wvnsprintfW(ptr long wstr ptr)
@   varargs -version=0x600+ ShellMessageBoxA(ptr ptr str str long) shell32.ShellMessageBoxA
