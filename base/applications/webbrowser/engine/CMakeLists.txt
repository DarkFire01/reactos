
# DefyBrowser engine: HTML, CSS, layout and paint, with GDI text and WinHTTP fetches
list(APPEND SOURCE
    src/core/DfBrSvg.c
    src/html/DfBrHtmlDom.c
    src/html/DfBrHtmlTags.c
    src/html/DfBrHtmlParse.c
    src/html/DfBrHtmlTree.c
    src/html/DfBrHtmlSerialize.c
    src/html/DfBrCanvas.c
    src/css/DfBrCssArena.c
    src/css/DfBrCssTokenize.c
    src/css/DfBrCssProps.c
    src/css/DfBrCssValue.c
    src/css/DfBrCssParse.c
    src/css/DfBrCssSelector.c
    src/css/DfBrCssMatch.c
    src/css/DfBrCssComputed.c
    src/css/DfBrCssCascade.c
    src/css/DfBrCssMedia.c
    src/css/DfBrCssUserAgent.c
    src/css/DfBrCssSerialize.c
    src/layout/DfBrLayoutArena.c
    src/layout/DfBrLayoutBuild.c
    src/layout/DfBrLayoutLength.c
    src/layout/DfBrLayoutBlock.c
    src/layout/DfBrLayoutInline.c
    src/layout/DfBrLayoutFloat.c
    src/layout/DfBrLayoutFlex.c
    src/layout/DfBrLayoutPositioned.c
    src/layout/DfBrLayoutSerialize.c
    src/paint/DfBrPaint.c
    src/image/DfBrImage.c
    src/net/DfBrNetUrl.c
    src/net/DfBrCookies.c
    src/net/win32_winhttp/DfBrNetWinHttp.c
    src/font/gdi/DfBrFontGdi.c)

add_library(dfbrengine STATIC ${SOURCE})
target_include_directories(dfbrengine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(dfbrengine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/nanosvg)
target_compile_definitions(dfbrengine PRIVATE DFBR_NO_WEBP _CRT_SECURE_NO_WARNINGS)

if(MSVC)
    # Portable C99 written to its own warning policy, not the tree's
    target_compile_options(dfbrengine PRIVATE /W0)
else()
    target_compile_options(dfbrengine PRIVATE -w)
endif()

add_dependencies(dfbrengine psdk)
