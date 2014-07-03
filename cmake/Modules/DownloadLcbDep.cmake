# DownloadLcbDeb(url location)
MACRO(DOWNLOAD_LCB_DEP url dest)
    IF(UNIX)
        MESSAGE(STATUS "Downloading ${url} => ${dest}")
        EXECUTE_PROCESS(COMMAND wget -O "${dest}" "${url}")
    ELSEIF(WIN32)
        EXECUTE_PROCESS(COMMAND powershell -Command
            "(New-Object Net.WebClient).DownloadFile('${url}', '${dest}')")
    ELSE()
        MESSAGE(WARNING "Using buggy built-in CMake downloader")
        FILE(DOWNLOAD ${url} ${dest} INACTIVITY_TIMEOUT 30 SHOW_PROGRESS)
    ENDIF()
ENDMACRO()
