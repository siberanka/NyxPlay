# NSIS Packaging
# see options at: https://cmake.org/cmake/help/latest/cpack_gen/nsis.html

# Escaping helpers. The values travel through CPack twice: a literal backslash is written as two
# backslashes and a double quote as a backslash followed by a quote.
set(NYX_BS "\\\\")
set(NYX_Q "\\\"")
set(NYX_NL "$${NYX_BS}r$${NYX_BS}n")
set(NYX_SYS "${NYX_Q}$SYSDIR${NYX_BS}")
set(NYX_SCRIPTS "${NYX_Q}$INSTDIR${NYX_BS}scripts${NYX_BS}")
string(REPLACE "/" "${NYX_BS}" NYX_STOP_HOST_SOURCE "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/service/stop-host.bat")
set(NYX_POWERSHELL "${NYX_Q}$SYSDIR${NYX_BS}WindowsPowerShell${NYX_BS}v1.0${NYX_BS}powershell.exe${NYX_Q} -NoProfile -NonInteractive -ExecutionPolicy Bypass")

# Unicode installer (non-ASCII user names and folders): has to be set before the script starts
find_program(NYX_MAKENSIS makensis)
if(NYX_MAKENSIS)
    # makensis cannot be told to build a Unicode installer from inside the script (the setting has to
    # come first), so CPack runs it through a small wrapper that adds the option
    file(TO_NATIVE_PATH "${NYX_MAKENSIS}" NYX_MAKENSIS_NATIVE)
    set(NYX_MAKENSIS_WRAPPER "${CMAKE_BINARY_DIR}/makensis-unicode.cmd")
    file(WRITE "${NYX_MAKENSIS_WRAPPER}" "@\"${NYX_MAKENSIS_NATIVE}\" \"/XUnicode true\" %*
")
    set(CPACK_NSIS_EXECUTABLE "${NYX_MAKENSIS_WRAPPER}")
endif()

# The release version without a local build suffix, for the comparison with an installed version
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+" NYX_VER_NUM "${PROJECT_VERSION}")
if(NOT NYX_VER_NUM)
    set(NYX_VER_NUM "${PROJECT_VERSION}")
endif()

# Localised messages (English and Turkish), one copy for the installer and one for the uninstaller
set(NYX_STRINGS_TEMPLATE "
        Function NYX_FN_NAME
          StrCmp $LANGUAGE 1055 0 english
          StrCpy $NYX_MSG_IMPORT 'Bir Apollo veya Sunshine kurulumu bulundu. Ayarları, uygulamaları ve eşleştirilmiş cihazları ${PROJECT_DISPLAY_NAME} içine aktarmak istiyor musunuz?${NYX_NL}${NYX_NL}İki host aynı bağlantı noktalarını kullanamaz: sonrasında diğer hostu durdurun veya kaldırın ya da ayarlardan bağlantı noktasını değiştirin.'
          StrCpy $NYX_MSG_OLD '${PROJECT_DISPLAY_NAME} zaten kurulu. Yenisini kurmadan önce eski sürüm kaldırılsın mı?'
          StrCpy $NYX_MSG_FAILED 'Kurulum sırasında bazı adımlar tamamlanamadı. Ayrıntılar için kurulum günlüğüne bakın:'
          StrCpy $NYX_MSG_GAMEPAD 'Sanal Oyun Kumandası (ViGEmBus) kaldırılsın mı?'
          StrCpy $NYX_MSG_SUDOVDA 'SudoVDA Sanal Ekran Sürücüsü kaldırılsın mı? Apollo veya başka bir host hâlâ kullanıyorsa kaldırmayın.'
          StrCpy $NYX_MSG_DELETE '$INSTDIR${NYX_BS}config içindeki yapılandırma, kimlik bilgileri ve kapak görselleri kaldırılsın mı?'
          StrCpy $NYX_MSG_WELCOME_UPDATE '${PROJECT_DISPLAY_NAME} $NYX_OLD_VER kurulu. Kurulum, ayarlarınızı ve eşleştirilmiş istemcileri koruyarak onu ${NYX_VER_NUM} sürümüne güncelleyecek.${NYX_NL}${NYX_NL}Devam etmek için İleri düğmesine tıklayın.'
          StrCpy $NYX_MSG_WELCOME_REPAIR '${PROJECT_DISPLAY_NAME} $NYX_OLD_VER zaten kurulu. Kurulum, ayarlarınızı ve eşleştirilmiş istemcileri koruyarak onu onaracak veya yeniden kuracak.${NYX_NL}${NYX_NL}Devam etmek için İleri düğmesine tıklayın.'
          StrCpy $NYX_MSG_WELCOME_DOWNGRADE '${PROJECT_DISPLAY_NAME} için daha yeni bir sürüm ($NYX_OLD_VER) kurulu. Kurulum, onu daha eski olan ${NYX_VER_NUM} sürümüyle değiştirecek. Ayarlarınız ve eşleştirilmiş istemciler korunur, ancak yeni sürümün kaydettiği ayarlar çalışmayabilir.${NYX_NL}${NYX_NL}Yeni sürümü korumak için İptal düğmesine tıklayın.'
          Return
        english:
          StrCpy $NYX_MSG_IMPORT 'An Apollo or Sunshine installation was found. Import its settings, applications and paired devices into ${PROJECT_DISPLAY_NAME}?${NYX_NL}${NYX_NL}Both hosts cannot use the same ports: stop or uninstall the other host afterwards, or change the port in the settings.'
          StrCpy $NYX_MSG_OLD '${PROJECT_DISPLAY_NAME} is already installed. Do you want to remove the old version before installing the new one?'
          StrCpy $NYX_MSG_FAILED 'Some installation steps did not complete. See the installation log for details:'
          StrCpy $NYX_MSG_GAMEPAD 'Do you want to remove Virtual Gamepad?'
          StrCpy $NYX_MSG_SUDOVDA 'Do you want to remove the SudoVDA Virtual Display Driver? Keep it if Apollo or another host still uses it.'
          StrCpy $NYX_MSG_DELETE 'Do you want to remove the configuration, credentials and cover images in $INSTDIR${NYX_BS}config?'
          StrCpy $NYX_MSG_WELCOME_UPDATE '${PROJECT_DISPLAY_NAME} $NYX_OLD_VER is installed. Setup will update it to ${NYX_VER_NUM} and keep your settings and paired devices.${NYX_NL}${NYX_NL}Click Next to continue.'
          StrCpy $NYX_MSG_WELCOME_REPAIR '${PROJECT_DISPLAY_NAME} $NYX_OLD_VER is already installed. Setup will repair or reinstall it and keep your settings and paired devices.${NYX_NL}${NYX_NL}Click Next to continue.'
          StrCpy $NYX_MSG_WELCOME_DOWNGRADE 'A newer version of ${PROJECT_DISPLAY_NAME} ($NYX_OLD_VER) is installed. Setup will replace it with the older ${NYX_VER_NUM}. Your settings and paired devices are kept, but settings saved by the newer version may not work.${NYX_NL}${NYX_NL}Click Cancel to keep the newer version.'
        FunctionEnd
        ")
string(REPLACE "NYX_FN_NAME" "NyxInitStrings" NYX_STRINGS_INSTALL "${NYX_STRINGS_TEMPLATE}")
string(REPLACE "NYX_FN_NAME" "un.NyxInitStrings" NYX_STRINGS_UNINSTALL "${NYX_STRINGS_TEMPLATE}")

# The welcome page shows what the setup will do when a version is already installed, and the folder page is
# skipped then (the existing folder is used). Both hooks have to be defined right before their page macro.
string(REPLACE "/" "${NYX_BS}" NYX_LICENSE_NATIVE "${CPACK_RESOURCE_FILE_LICENSE}")
set(CPACK_NSIS_INSTALLER_WELCOME_TITLE_CODE "!define MUI_PAGE_CUSTOMFUNCTION_SHOW NyxWelcomeShow")
set(CPACK_NSIS_LICENSE_PAGE "!insertmacro MUI_PAGE_LICENSE ${NYX_Q}${NYX_LICENSE_NATIVE}${NYX_Q}
        !define MUI_PAGE_CUSTOMFUNCTION_PRE NyxDirPre")

# Global definitions (inserted at the top level of the script through the components page hook,
# the generator does not pass CPACK_NSIS_DEFINES through): install folder validation, the helpers
# that evaluate the result of the install steps and the configuration import
set(CPACK_NSIS_PAGE_COMPONENTS
        "

        Var NYX_FAILED
        Var NYX_FOUND
        Var NYX_SOURCE
        Var NYX_MSG_IMPORT
        Var NYX_MSG_OLD
        Var NYX_MSG_FAILED
        Var NYX_MSG_GAMEPAD
        Var NYX_MSG_SUDOVDA
        Var NYX_MSG_DELETE
        Var NYX_MSG_WELCOME_UPDATE
        Var NYX_MSG_WELCOME_REPAIR
        Var NYX_MSG_WELCOME_DOWNGRADE
        Var NYX_WELCOME_TEXT
        Var NYX_OLD_VER
        Var NYX_OLD_DIR
        Var NYX_VERCMP
        Var NYX_UPDATE
        Var NYX_MODE_READY

        # Messages in English and Turkish (the language is chosen from the system language)
        ${NYX_STRINGS_INSTALL}
        ${NYX_STRINGS_UNINSTALL}

        # Appends the line on the stack to <config>/updates/install.log (only when the configuration folder exists,
        # so nothing is created before the folder permissions are set). The text never holds secrets.
        Function NyxLog
          Exch $R0
          Push $R1
          IfFileExists '$INSTDIR${NYX_BS}config${NYX_BS}*.*' 0 nyx_log_done
          CreateDirectory '$INSTDIR${NYX_BS}config${NYX_BS}updates'
          ClearErrors
          FileOpen $R1 '$INSTDIR${NYX_BS}config${NYX_BS}updates${NYX_BS}install.log' a
          IfErrors nyx_log_done
          FileSeek $R1 0 END
          FileWrite $R1 '$R0${NYX_NL}'
          FileClose $R1
        nyx_log_done:
          Pop $R1
          Pop $R0
        FunctionEnd

        # Finds an installed version through its uninstall entry: $NYX_OLD_DIR and $NYX_OLD_VER stay empty when there
        # is none. $NYX_VERCMP: 0 = same version, 1 = this setup is newer, 2 = this setup is older.
        Function NyxDetectInstalled
          StrCpy $NYX_OLD_VER ${NYX_Q}${NYX_Q}
          StrCpy $NYX_OLD_DIR ${NYX_Q}${NYX_Q}
          StrCpy $NYX_VERCMP '1'
          ReadRegStr $R0 HKLM 'Software${NYX_BS}Microsoft${NYX_BS}Windows${NYX_BS}CurrentVersion${NYX_BS}Uninstall${NYX_BS}${PROJECT_DISPLAY_NAME}' 'UninstallString'
          StrCmp $R0 '' nyx_det_done
          StrCpy $R1 $R0 1 0
          StrCmp $R1 '${NYX_Q}' 0 +2
          StrCpy $R0 $R0 '' 1
          StrCpy $R1 $R0 1 -1
          StrCmp $R1 '${NYX_Q}' 0 +2
          StrCpy $R0 $R0 -1
          StrLen $R1 '${NYX_BS}Uninstall.exe'
          StrCpy $R0 $R0 -$R1
          IfFileExists '$R0${NYX_BS}nyxplay-host.exe' 0 nyx_det_done
          StrCpy $NYX_OLD_DIR $R0
          ReadRegStr $NYX_OLD_VER HKLM 'Software${NYX_BS}Microsoft${NYX_BS}Windows${NYX_BS}CurrentVersion${NYX_BS}Uninstall${NYX_BS}${PROJECT_DISPLAY_NAME}' 'DisplayVersion'
          StrCmp $NYX_OLD_VER '' nyx_det_done
          # Natural order comparison: 2026.11.19.10 is newer than 2026.11.19.9
          StrCpy $R0 '${NYX_VER_NUM}'
          StrCpy $R1 $NYX_OLD_VER
          System::Call 'shlwapi::StrCmpLogicalW(w r10, w r11) i .r12'
          IntCmp $R2 0 nyx_det_same nyx_det_older nyx_det_newer
        nyx_det_same:
          StrCpy $NYX_VERCMP '0'
          Goto nyx_det_done
        nyx_det_older:
          StrCpy $NYX_VERCMP '2'
          Goto nyx_det_done
        nyx_det_newer:
          StrCpy $NYX_VERCMP '1'
        nyx_det_done:
        FunctionEnd

        # Reads the mode of this run once: an installed version and the /UPDATE switch
        Function NyxInitMode
          StrCmp $NYX_MODE_READY '1' nyx_mode_done
          StrCpy $NYX_MODE_READY '1'
          Call NyxDetectInstalled
          StrCpy $NYX_UPDATE '0'
          Push $CMDLINE
          Push '/UPDATE'
          Call StrStr
          Pop $R0
          StrCmp $R0 '' nyx_mode_done
          StrCpy $NYX_UPDATE '1'
        nyx_mode_done:
        FunctionEnd

        # Welcome page: says what happens when a version is already installed. The text control is found by its
        # content size (the welcome text is the longest static control of the page).
        Function NyxWelcomeShow
          Call NyxInitMode
          StrCmp $NYX_OLD_VER '' nyx_welcome_done
          Call NyxInitStrings
          StrCmp $NYX_VERCMP '0' nyx_welcome_same
          StrCmp $NYX_VERCMP '2' nyx_welcome_down
          StrCpy $NYX_WELCOME_TEXT $NYX_MSG_WELCOME_UPDATE
          Goto nyx_welcome_set
        nyx_welcome_same:
          StrCpy $NYX_WELCOME_TEXT $NYX_MSG_WELCOME_REPAIR
          Goto nyx_welcome_set
        nyx_welcome_down:
          StrCpy $NYX_WELCOME_TEXT $NYX_MSG_WELCOME_DOWNGRADE
        nyx_welcome_set:
          FindWindow $R0 '#32770' '' $HWNDPARENT
          StrCpy $R1 0
          StrCpy $R2 0
          StrCpy $R3 0
        nyx_welcome_scan:
          FindWindow $R1 'Static' '' $R0 $R1
          IntCmp $R1 0 nyx_welcome_found
          System::Call 'user32::GetWindowTextLengthW(i r11) i .r14'
          IntCmp $R4 $R3 nyx_welcome_scan nyx_welcome_scan nyx_welcome_better
        nyx_welcome_better:
          StrCpy $R3 $R4
          StrCpy $R2 $R1
          Goto nyx_welcome_scan
        nyx_welcome_found:
          IntCmp $R2 0 nyx_welcome_done
          SendMessage $R2 12 0 'STR:$NYX_WELCOME_TEXT'
        nyx_welcome_done:
        FunctionEnd

        # Folder page: an installed version is updated in its own folder, so the page is skipped
        Function NyxDirPre
          Call NyxInitMode
          StrCmp $NYX_OLD_DIR '' nyx_dirpre_done
          StrCpy $INSTDIR $NYX_OLD_DIR
          Abort
        nyx_dirpre_done:
        FunctionEnd

        # Critical step: a failure is logged, shown and makes the installer return a non-zero exit code
        Function NyxCritical
          StrCmp $0 '0' nyx_critical_ok
          DetailPrint 'FAILED: $1 (exit code $0)'
          Push 'FAILED: $1 (exit code $0)'
          Call NyxLog
          SetErrorLevel 2
          StrCpy $NYX_FAILED '$NYX_FAILED${NYX_NL}  - $1 ($0)'
        nyx_critical_ok:
        FunctionEnd

        # Optional step: a failure is only logged
        Function NyxOptional
          StrCmp $0 '0' nyx_optional_ok
          DetailPrint 'WARNING: $1 did not complete (exit code $0)'
          Push 'WARNING: $1 did not complete (exit code $0)'
          Call NyxLog
        nyx_optional_ok:
        FunctionEnd

        # Drive roots and system folders are not accepted as installation folder
        Function .onVerifyInstDir
          StrLen $0 '$INSTDIR'
          IntCmp $0 3 nyx_dir_bad nyx_dir_bad nyx_dir_check
        nyx_dir_check:
          StrCmp '$INSTDIR' '$WINDIR' nyx_dir_bad
          StrCmp '$INSTDIR' '$SYSDIR' nyx_dir_bad
          StrCmp '$INSTDIR' '$PROGRAMFILES' nyx_dir_bad
          StrCmp '$INSTDIR' '$PROGRAMFILES64' nyx_dir_bad
          StrCmp '$INSTDIR' '$PROFILE' nyx_dir_bad
          StrCmp '$INSTDIR' '$DESKTOP' nyx_dir_bad
          StrCmp '$INSTDIR' '$DOCUMENTS' nyx_dir_bad
          Return
        nyx_dir_bad:
          Abort
        FunctionEnd

        # A chosen folder that already holds other files (or a drive root) gets a product sub folder, so
        # the permissions set below and the uninstaller never touch files that are not ours
        Function NyxResolveInstDir
          IfFileExists '$INSTDIR${NYX_BS}nyxplay-host.exe' nyx_inst_done
          IfFileExists '$INSTDIR${NYX_BS}Uninstall.exe' nyx_inst_done
          IfFileExists '$INSTDIR${NYX_BS}config${NYX_BS}sunshine.conf' nyx_inst_done
          IfFileExists '$INSTDIR${NYX_BS}config${NYX_BS}apps.json' nyx_inst_done
          StrLen $0 '$INSTDIR'
          IntCmp $0 3 nyx_inst_append nyx_inst_append nyx_inst_scan
        nyx_inst_scan:
          FindFirst $0 $1 '$INSTDIR${NYX_BS}*.*'
        nyx_inst_loop:
          StrCmp $1 '' nyx_inst_empty
          StrCmp $1 '.' nyx_inst_next
          StrCmp $1 '..' nyx_inst_next
          FindClose $0
          Goto nyx_inst_append
        nyx_inst_next:
          FindNext $0 $1
          Goto nyx_inst_loop
        nyx_inst_empty:
          FindClose $0
          Goto nyx_inst_done
        nyx_inst_append:
          StrCpy $2 '$INSTDIR' 1 -1
          StrCmp $2 '${NYX_BS}' 0 nyx_inst_join
          StrCpy $INSTDIR '$INSTDIR${PROJECT_DISPLAY_NAME}'
          Goto nyx_inst_log
        nyx_inst_join:
          StrCpy $INSTDIR '$INSTDIR${NYX_BS}${PROJECT_DISPLAY_NAME}'
        nyx_inst_log:
          DetailPrint 'The selected folder is not empty, installing into $INSTDIR'
        nyx_inst_done:
        FunctionEnd

        # Looks for the configuration of an Apollo or Sunshine installation through its uninstall entry
        # (covers custom install folders and the old layout with sunshine.conf in the install folder)
        Function NyxFindOtherConfig
          StrCpy $NYX_SOURCE ${NYX_Q}${NYX_Q}
          StrCpy $R1 'Apollo'
        nyx_other_loop:
          ReadRegStr $R0 HKLM 'Software${NYX_BS}Microsoft${NYX_BS}Windows${NYX_BS}CurrentVersion${NYX_BS}Uninstall${NYX_BS}$R1' 'UninstallString'
          StrCmp $R0 '' nyx_other_next
          StrCpy $R2 $R0 1 0
          StrCmp $R2 '${NYX_Q}' 0 +2
          StrCpy $R0 $R0 '' 1
          StrCpy $R2 $R0 1 -1
          StrCmp $R2 '${NYX_Q}' 0 +2
          StrCpy $R0 $R0 -1
          StrLen $R2 '${NYX_BS}Uninstall.exe'
          StrCpy $R0 $R0 -$R2
          IfFileExists '$R0${NYX_BS}config${NYX_BS}sunshine.conf' 0 +3
          StrCpy $NYX_SOURCE '$R0${NYX_BS}config'
          Goto nyx_other_done
          IfFileExists '$R0${NYX_BS}sunshine.conf' 0 nyx_other_next
          StrCpy $NYX_SOURCE '$R0'
          Goto nyx_other_done
        nyx_other_next:
          StrCmp $R1 'Sunshine' nyx_other_done
          StrCpy $R1 'Sunshine'
          Goto nyx_other_loop
        nyx_other_done:
        FunctionEnd

        # First installation only: offer to import the configuration of an installed Apollo or Sunshine
        # host (runs before the service starts; the other installation is not modified). Silent
        # installations import only when /IMPORT=1 is given.
        Function NyxImportConfig
          IfFileExists '$INSTDIR${NYX_BS}config${NYX_BS}sunshine.conf' nyx_import_done
          StrCpy $NYX_SOURCE ${NYX_Q}${NYX_Q}
          nsExec::ExecToStack /TIMEOUT=30000 '${NYX_Q}$INSTDIR${NYX_BS}nyxplay-host.exe${NYX_Q} --import-config detect'
          Pop $0
          Pop $NYX_FOUND
          StrCmp $0 '0' nyx_import_detected
          Call NyxFindOtherConfig
          StrCmp $NYX_SOURCE '' nyx_import_done
          StrCpy $NYX_FOUND 'Found configuration: $NYX_SOURCE'
        nyx_import_detected:
          IfSilent nyx_import_silent nyx_import_ask
        nyx_import_silent:
          Push $CMDLINE
          Push '/IMPORT=1'
          Call StrStr
          Pop $3
          StrCmp $3 '' nyx_import_done nyx_import_run
        nyx_import_ask:
          MessageBox MB_YESNO|MB_ICONQUESTION '$NYX_MSG_IMPORT${NYX_NL}${NYX_NL}$NYX_FOUND' IDYES nyx_import_run IDNO nyx_import_done
        nyx_import_run:
          DetailPrint 'Importing: $NYX_FOUND'
          StrCmp $NYX_SOURCE '' 0 nyx_import_explicit
          nsExec::ExecToLog /TIMEOUT=120000 '${NYX_Q}$INSTDIR${NYX_BS}nyxplay-host.exe${NYX_Q} --import-config auto'
          Goto nyx_import_result
        nyx_import_explicit:
          nsExec::ExecToLog /TIMEOUT=120000 '${NYX_Q}$INSTDIR${NYX_BS}nyxplay-host.exe${NYX_Q} --import-config ${NYX_Q}$NYX_SOURCE${NYX_Q}'
        nyx_import_result:
          Pop $0
          StrCpy $1 'Import of the existing configuration'
          Call NyxOptional
        nyx_import_done:
        FunctionEnd

        # Runs before the files of the new version are copied (the component sections follow): reads the start type
        # of the service of a previous version (a value left by an earlier uninstall is dropped first), then
        # removes that version
        Section '-Previous version'
        Call NyxInitStrings
        Call NyxInitMode
        # /UPDATE: the installed folder is used and nothing is asked; an older setup never replaces a newer version
        StrCmp $NYX_UPDATE '1' 0 nyx_mode_ready
        StrCmp $NYX_OLD_DIR '' nyx_mode_ready
        StrCpy $INSTDIR $NYX_OLD_DIR
        StrCmp $NYX_VERCMP '2' 0 nyx_mode_ready
        Push 'Refused: version $NYX_OLD_VER is installed and this setup is ${NYX_VER_NUM}'
        Call NyxLog
        SetErrorLevel 5
        Abort
        nyx_mode_ready:
        Push 'Setup ${NYX_VER_NUM}: installed version [$NYX_OLD_VER], update mode $NYX_UPDATE'
        Call NyxLog
        SetRegView 64
        DeleteRegValue HKLM 'Software${NYX_BS}NyxPlay Host' 'ServiceStartType'
        ClearErrors
        ReadRegDWORD $0 HKLM 'SYSTEM${NYX_BS}CurrentControlSet${NYX_BS}Services${NYX_BS}NyxPlayHostService' 'Start'
        IfErrors nyx_st_done
        StrCmp $0 2 nyx_st_auto
        StrCmp $0 3 nyx_st_write
        StrCmp $0 4 nyx_st_write
        Goto nyx_st_done
        nyx_st_auto:
        ClearErrors
        ReadRegDWORD $1 HKLM 'SYSTEM${NYX_BS}CurrentControlSet${NYX_BS}Services${NYX_BS}NyxPlayHostService' 'DelayedAutostart'
        IfErrors nyx_st_write
        StrCmp $1 1 0 nyx_st_write
        StrCpy $0 '2-delayed'
        nyx_st_write:
        WriteRegStr HKLM 'Software${NYX_BS}NyxPlay Host' 'ServiceStartType' '$0'
        nyx_st_done:
        SetRegView default
        ReadRegStr $0 HKLM 'Software${NYX_BS}Microsoft${NYX_BS}Windows${NYX_BS}CurrentVersion${NYX_BS}Uninstall${NYX_BS}${PROJECT_DISPLAY_NAME}' 'UninstallString'
        StrCmp $0 '' nyx_old_done
        StrCpy $2 $0 1 0
        StrCmp $2 '${NYX_Q}' 0 +2
        StrCpy $0 $0 '' 1
        StrCpy $2 $0 1 -1
        StrCmp $2 '${NYX_Q}' 0 +2
        StrCpy $0 $0 -1
        IfFileExists '$0' 0 nyx_old_done
        StrLen $2 '${NYX_BS}Uninstall.exe'
        StrCpy $3 $0 -$2
        # An update or a repair is what the welcome page announced: no question. Only an unknown or a newer
        # installed version is asked about.
        StrCmp $NYX_OLD_VER '' nyx_old_ask
        StrCmp $NYX_VERCMP '2' nyx_old_ask
        Goto nyx_old_run
        nyx_old_ask:
        MessageBox MB_YESNOCANCEL|MB_ICONEXCLAMATION '$NYX_MSG_OLD' /SD IDYES IDYES nyx_old_run IDNO nyx_old_done
        Abort
        nyx_old_run:
        ClearErrors
        ExecWait '${NYX_Q}$0${NYX_Q} /S _?=$3' $0
        StrCpy $1 'Removing the previous version'
        Call NyxOptional
        nyx_old_done:
        SectionEnd

        !insertmacro MUI_PAGE_COMPONENTS
        ")

# Extra pre-install commands (run before any file is copied): choose a safe install folder and end
# a running host that would keep its files locked
set(CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS
        "${CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS}
        Call NyxInitStrings
        Call NyxResolveInstDir
        SetOutPath '$INSTDIR'
        InitPluginsDir
        File '/oname=$PLUGINSDIR${NYX_BS}stop-host.bat' '${NYX_STOP_HOST_SOURCE}'
        nsExec::ExecToLog /TIMEOUT=90000 '${NYX_Q}$PLUGINSDIR${NYX_BS}stop-host.bat${NYX_Q}'
        Pop $0
        StrCpy $1 'Stopping the running host'
        Call NyxOptional
        ")

# Extra install commands
# Restores permissions on the install directory
# Migrates config files from the root into the new config folder
# Install service
# Every step reports its result: critical steps make the installer return a non-zero exit code
SET(CPACK_NSIS_EXTRA_INSTALL_COMMANDS
        "${CPACK_NSIS_EXTRA_INSTALL_COMMANDS}
        StrCpy $NYX_FAILED ${NYX_Q}${NYX_Q}
        nsExec::ExecToLog /TIMEOUT=60000 '${NYX_SYS}icacls.exe${NYX_Q} ${NYX_Q}$INSTDIR${NYX_Q} /reset'
        Pop $0
        StrCpy $1 'Resetting the install folder permissions'
        Call NyxCritical
        nsExec::ExecToLog /TIMEOUT=60000 '${NYX_SYS}icacls.exe${NYX_Q} ${NYX_Q}$INSTDIR${NYX_Q} /grant:r *S-1-5-18:(OI)(CI)(F) *S-1-5-32-544:(OI)(CI)(F) *S-1-5-32-545:(OI)(CI)(RX) /C /Q'
        Pop $0
        StrCpy $1 'Setting the install folder permissions'
        Call NyxCritical
        nsExec::ExecToLog /TIMEOUT=60000 '${NYX_SYS}icacls.exe${NYX_Q} ${NYX_Q}$INSTDIR${NYX_Q} /inheritance:r /C /Q'
        Pop $0
        StrCpy $1 'Restricting the install folder permissions'
        Call NyxCritical
        nsExec::ExecToLog /TIMEOUT=120000 '${NYX_SCRIPTS}update-path.bat${NYX_Q} add'
        Pop $0
        StrCpy $1 'Adding NyxPlay Host to the system PATH'
        Call NyxOptional
        nsExec::ExecToLog /TIMEOUT=300000 '${NYX_Q}$INSTDIR${NYX_BS}drivers${NYX_BS}sudovda${NYX_BS}install.bat${NYX_Q}'
        Pop $0
        StrCpy $1 'Installing the SudoVDA virtual display driver'
        Call NyxOptional
        nsExec::ExecToLog /TIMEOUT=120000 '${NYX_SCRIPTS}migrate-config.bat${NYX_Q}'
        Pop $0
        StrCpy $1 'Migrating the configuration'
        Call NyxCritical
        Call NyxImportConfig
        IfFileExists '$INSTDIR${NYX_BS}scripts${NYX_BS}check-other-hosts.bat' 0 nyx_no_hostcheck
        nsExec::ExecToLog /TIMEOUT=30000 '${NYX_SCRIPTS}check-other-hosts.bat${NYX_Q}'
        Pop $0
        nyx_no_hostcheck:
        IfFileExists '$INSTDIR${NYX_BS}scripts${NYX_BS}add-firewall-rule.bat' 0 nyx_no_firewall
        nsExec::ExecToLog /TIMEOUT=60000 '${NYX_SCRIPTS}add-firewall-rule.bat${NYX_Q}'
        Pop $0
        StrCpy $1 'Adding the firewall rules'
        Call NyxCritical
        nyx_no_firewall:
        IfFileExists '$INSTDIR${NYX_BS}scripts${NYX_BS}install-gamepad.ps1' 0 nyx_no_gamepad
        StrCpy $2 ${NYX_Q}${NYX_Q}
        IfSilent 0 +2
        StrCpy $2 ' -Silent'
        nsExec::ExecToLog /TIMEOUT=300000 '${NYX_POWERSHELL} -File ${NYX_SCRIPTS}install-gamepad.ps1${NYX_Q}$2'
        Pop $0
        StrCpy $1 'Installing the virtual gamepad driver'
        Call NyxOptional
        nyx_no_gamepad:
        nsExec::ExecToLog /TIMEOUT=120000 '${NYX_SCRIPTS}install-service.bat${NYX_Q}'
        Pop $0
        StrCpy $1 'Installing and starting the host service'
        Call NyxCritical
        # The script exists only when Run at Startup is selected in this installation. A component that was not
        # installed before was switched on now (or this is a first installation): that choice always wins. When it was
        # already installed, a start type the user changed since is kept (see autostart-service.bat).
        IfFileExists '$INSTDIR${NYX_BS}scripts${NYX_BS}autostart-service.bat' 0 nyx_no_autostart
        IntCmp $autostart_was_installed 0 nyx_autostart_force nyx_autostart_keep nyx_autostart_keep
        nyx_autostart_force:
        nsExec::ExecToLog /TIMEOUT=60000 '${NYX_SCRIPTS}autostart-service.bat${NYX_Q} force'
        Goto nyx_autostart_done
        nyx_autostart_keep:
        nsExec::ExecToLog /TIMEOUT=60000 '${NYX_SCRIPTS}autostart-service.bat${NYX_Q}'
        nyx_autostart_done:
        Pop $0
        StrCpy $1 'Enabling launch on startup'
        Call NyxOptional
        nyx_no_autostart:
        StrCmp $NYX_FAILED '' nyx_install_ok
        IfSilent nyx_install_ok 0
        MessageBox MB_OK|MB_ICONEXCLAMATION '$NYX_MSG_FAILED$NYX_FAILED'
        nyx_install_ok:
        StrCmp $NYX_FAILED '' 0 nyx_result_failed
        Push 'Setup completed: ${NYX_VER_NUM}'
        Call NyxLog
        Goto nyx_result_done
        nyx_result_failed:
        Push 'Setup finished with errors:$NYX_FAILED'
        Call NyxLog
        nyx_result_done:
        ")

# Extra uninstall commands
# Uninstall service
set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
        "${CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS}
        Call un.NyxInitStrings
        nsExec::ExecToLog /TIMEOUT=60000 '${NYX_SCRIPTS}delete-firewall-rule.bat${NYX_Q}'
        Pop $0
        # Stops the service and any host process that is still running, so no file stays locked
        nsExec::ExecToLog /TIMEOUT=120000 '${NYX_SCRIPTS}uninstall-service.bat${NYX_Q}'
        Pop $0
        nsExec::ExecToLog /TIMEOUT=60000 '${NYX_Q}$INSTDIR${NYX_BS}nyxplay-host.exe${NYX_Q} --restore-nvprefs-undo'
        Pop $0
        # Must run before $INSTDIR can be deleted below and regardless of the keep-config answer
        nsExec::ExecToLog /TIMEOUT=120000 '${NYX_SCRIPTS}update-path.bat${NYX_Q} remove'
        Pop $0
        MessageBox MB_YESNO|MB_ICONQUESTION \
            '$NYX_MSG_GAMEPAD' \
            /SD IDNO IDNO NoGamepad
            nsExec::ExecToLog /TIMEOUT=360000 '${NYX_POWERSHELL} -File ${NYX_SCRIPTS}uninstall-gamepad.ps1${NYX_Q}'
            Pop $0
        NoGamepad:
        MessageBox MB_YESNO|MB_ICONQUESTION \
            '$NYX_MSG_SUDOVDA' \
            /SD IDNO IDNO NoSudoVDA
            nsExec::ExecToLog /TIMEOUT=300000 '${NYX_Q}$INSTDIR${NYX_BS}drivers${NYX_BS}sudovda${NYX_BS}uninstall.bat${NYX_Q}'
            Pop $0
        NoSudoVDA:
        # Only the data the host created is removed here; the files of the installation itself are
        # removed individually below, so unrelated files in the folder are never deleted
        MessageBox MB_YESNO|MB_ICONQUESTION \
            '$NYX_MSG_DELETE' \
            /SD IDNO IDNO NoDelete
            RMDir /r '$INSTDIR${NYX_BS}config'
            RMDir /r '$INSTDIR${NYX_BS}credentials'
            RMDir /r '$INSTDIR${NYX_BS}covers'
        NoDelete:
        ")

# Adding an option for the start menu
set(CPACK_NSIS_MODIFY_PATH OFF)
set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
# This will be shown on the installed apps Windows settings
set(CPACK_NSIS_INSTALLED_ICON_NAME "nyxplay-host.exe")
set(CPACK_NSIS_CREATE_ICONS_EXTRA
        "${CPACK_NSIS_CREATE_ICONS_EXTRA}
        SetOutPath '\$INSTDIR'
        CreateShortCut '\$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\${PROJECT_DISPLAY_NAME}.lnk' \
            '\$INSTDIR\\\\nyxplay-host.exe' '--shortcut'
        ")
set(CPACK_NSIS_DELETE_ICONS_EXTRA
        "${CPACK_NSIS_DELETE_ICONS_EXTRA}
        Delete '\$SMPROGRAMS\\\\$MUI_TEMP\\\\${PROJECT_DISPLAY_NAME}.lnk'
        ")

# Previous versions are removed by the '-Previous version' section above, after the start type of the service was read
# (the generator's own step runs too early for that)
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL "OFF")

# set(CPACK_NSIS_HELP_LINK "https://docs.lizardbyte.dev/projects/sunshine/latest/md_docs_2getting__started.html")
# set(CPACK_NSIS_URL_INFO_ABOUT "${CMAKE_PROJECT_HOMEPAGE_URL}")
# set(CPACK_NSIS_CONTACT "${CMAKE_PROJECT_HOMEPAGE_URL}/support")

# set(CPACK_NSIS_MENU_LINKS
#         "https://docs.lizardbyte.dev/projects/sunshine" "Sunshine documentation"
#         "https://app.lizardbyte.dev" "LizardByte Web Site"
#         "https://app.lizardbyte.dev/support" "LizardByte Support")
set(CPACK_NSIS_MANIFEST_DPI_AWARE true)
