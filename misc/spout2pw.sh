#!/usr/bin/env bash
set -E

spout2pw="$(dirname "$(realpath "$0")")"

setup_logging() {
    zenity=
    kdialog=
    if [ -n "$WAYLAND_DISPLAY$DISPLAY" ] ; then
        zenity="$(which zenity 2>/dev/null || true)"
        kdialog="$(which kdialog 2>/dev/null || true)"
    fi
    if [ -n "$zenity" ] && [ -e "$zenity" ]; then
        show_info() {
            echo -e "** Spout2PW info: $*"
            [ "$quiet" == 1 ] && return
            $zenity --width=600 --title="Spout2PW" --info --text="$*"
        }
        show_warning() {
            echo -e "** Spout2PW warning: $*"
            [ "$quiet" == 1 ] && return
            $zenity --width=600 --title="Spout2PW warning" --warning --text="$*"
        }
        show_error() {
            echo -e "** Spout2PW error: $*"
            [ "$quiet" == 1 ] && return
            $zenity --width=600 --title="Spout2PW error" --error --text="$*"
        }
    elif [ -n "$kdialog" ] && [ -e "$kdialog" ]; then
        show_info() {
            echo -e "** Spout2PW info: $*"
            [ "$quiet" == 1 ] && return
            $kdialog --title="Spout2PW" --msgbox "$*"
        }
        show_warning() {
            echo -e "** Spout2PW warning: $*"
            [ "$quiet" == 1 ] && return
            $kdialog --title="Spout2PW warning" --sorry "$*"
        }
        show_error() {
            echo -e "** Spout2PW error: $*"
            [ "$quiet" == 1 ] && return
            $kdialog --title="Spout2PW error" --sorry "$*"
        }
    else
        show_info() {
            echo -e "** Spout2PW info: $*"
        }
        show_warning() {
            echo -e "** Spout2PW warning: $*"
        }
        show_error() {
            echo -e "** Spout2PW error: $*"
        }
    fi

    log() {
        echo "Spout2PW(debug): $*"
    }

    fatal() {
        show_error "$@"
        exit 1
    }

    trap 'fatal "Unexpected error on line $LINENO"' ERR
}

check_environment() {
    flatpak=0

    if [ -e /.flatpak-info ]; then
        flatpak=1
    fi
}

verchk() {
    oldest=$(printf '%s\n' "$2" "$1" | sort -V | head -n1)
    log "  Version check: $1 >= $2, oldest: $oldest"
    [ "$oldest" = "$2" ] || return 1
}

check_pipewire() {
    pw_version="$(pw-dump | grep '"version": "' 2>/dev/null | head -n 1 | cut -d: -f2 | tr -d '", ')"
    [ -z "$pw_version" ] && fatal "Failed to check PipeWire daemon version"
    log "PipeWire version: $pw_version"

    if ! verchk "$pw_version" 1.2.7; then
        fatal "Your PipeWire version is too old ($pw_version).\n\nThe minimum supported version is 1.2.7.\n\nVisit lina.yt/oldpw for more info."
    fi
}

find_gbm_backends() {
    gbm_backend_paths="
        /usr/lib/x86_64-linux-gnu/GL/lib
        /usr/lib/x86_64-linux-gnu
        /usr/lib64
        /lib64
        /usr/lib
        /lib
    "

    gbm_backends=

    for libdir in $gbm_backend_paths; do
        if [ -d "$libdir"/gbm ]; then
            gbm_backends=$libdir/gbm
            break
        fi
    done

    if [ ! -d "$gbm_backends"  ]; then
        fatal "Failed to find GBM backend path"
    fi

    log "GBM backend path: $gbm_backends"
}

gbm_steamrt_workaround() {
    if [ -z "$1" ] || [ ! -d "$1" ] || [ ! -e "$1/VERSIONS.txt" ]; then
        log "Could not find Steam Runtime VERSIONS.txt at '$1', not checking for GBM workaround"
        return
    fi

    srt_version="$(cat "$1"/VERSIONS.txt | grep '^pressure-vessel' | cut -f2)"
    log "Steam Runtime version: $srt_version"
    if [ -z "$srt_version" ]; then
        log "  Invalid Steam Runtime version, assuming new and skipping GBM workaround"
        return
    fi
    if verchk "$srt_version" 0.20260218.0; then
        log "  Steam Runtime is new enough, skipping GBM workaround"
        return
    fi

    log "  Steam Runtime is old, applying GBM workaround"
    log "Staging GBM backends..."

    gbm_staging="$(mktemp --tmpdir=/tmp -d spout2pw-gbm.XXXXXXXXXX)"
    [ ! -d "$gbm_staging" ] && fatal "Failed to create staging directory for GBM backends"
    gbm_staging="$(realpath "$gbm_staging")"

    log "GBM backend staging path: $gbm_staging"

    trap "rm -vrf $gbm_staging" 1 2 3 6 15 EXIT

    for i in $gbm_backends/*; do
        base="$(basename "$i")"
        log "Staging GBM backend $base:"
        rp="$(realpath "$i")"
        if [ "$flatpak" = 1 ]; then
            src="/run/parent$rp"
        else
            src="/run/host$rp"
        fi
        dst="$gbm_staging/$base"
        log "  Linking $dst -> $src"
        ln -s "$src" "$dst"
    done

    export GBM_BACKENDS_PATH="$gbm_staging"
}

setup_wine() {
    fatal "Vanilla wine is not supported yet!"

    wineprefix="$WINEPREFIX"
}

setup_umu() {
    umu="$1"
    if [ -z "$PROTONPATH" ]; then
        export PROTONPATH="GE-Proton"
    fi

    log "Setting up Proton with umu-launcher ($PROTONPATH)..."

    readarray -t umu_vars <<<"$(UMU_LOG=1 "$umu" /bin/true 2>&1 | grep 'umu_run:.*=' | sed 's/.*: //')"

    for var in "${umu_vars[@]}"; do
        key="$(echo "$var" | cut -d= -f1)"
        val="$(echo "$var" | cut -d= -f2-)"
        case "$key" in
            PROTONPATH)
                protonpath="$val"
                ;;
            RUNTIMEPATH)
                runtimepath="$val"
                ;;
        esac
    done

    if [ "$UMU_NO_RUNTIME" != 1 ]; then
        gbm_steamrt_workaround "$runtimepath"
    fi

    if [ -n "$WINEPREFIX" ]; then
        wineprefix="$WINEPREFIX"
    elif [ -n "$GAMEID" ]; then
        wineprefix="$HOME/Games/umu/$GAMEID/"
    else
        wineprefix="$HOME/Games/umu/umu-default/"
    fi

    run_in_prefix() {
        log "run_in_prefix: $@"

        PROTON_VERB=run \
        WINEPREFIX="$wineprefix" \
        PROTONPATH="$protonpath" "$umu" "$@"
    }
}

setup_steam() {
    wineprefix="$STEAM_COMPAT_DATA_PATH/pfx"
    protonpath="$(echo "$STEAM_COMPAT_TOOL_PATHS" | cut -d: -f1)"

    launch_cmd=()
    steam_runtime=0
    for arg in "$@"; do
        log "Arg: $arg"
        launch_cmd+=("$arg")

        [[ "$arg" == *SteamLinuxRuntime* ]] && steam_runtime=1
        [[ "$arg" == */proton ]] && break
    done

    log "Steam launch mode: flatpak=$flatpak steam_runtime=$steam_runtime"

    if [ "$flatpak" = 1 ]; then
        log "Working around Flatpak Steam LD_AUDIT issue"
        unset LD_AUDIT
    fi

    if [[ ! "$1" == */proton ]] && [ "$steam_runtime" = 1 ]; then
        runtimepath="$(echo "$STEAM_COMPAT_TOOL_PATHS" | cut -d: -f2)"
        gbm_steamrt_workaround "$runtimepath"
    fi

    log "Steam Proton launch command: ${launch_cmd[@]}"

    run_in_prefix() {
        log "run_in_prefix: $@"
        "${launch_cmd[@]}" run "$@"
    }
}

usage() {
    show_info \
"Usage:

   $0 umu-run app.exe

or set the Steam launch flags to:

   $(realpath "$0") %command%"
}

validate_paths() {
    log "Wine prefix: $wineprefix"
    if [ ! -d "$wineprefix/dosdevices" ]; then
        fatal "Could not find wine prefix at '$wineprefix'"
    fi
    cdrive="$wineprefix/dosdevices/c:/"
    system32="$cdrive/windows/system32"
    if [ ! -d "$system32" ]; then
        fatal "Could not find System32 at '$system32'"
    fi

    log "Proton path: $protonpath"
    if [ ! -e "$protonpath/proton" ]; then
        fatal "Could not find proton at '$protonpath'"
    fi
}

check_spout2pw_install() {
    check_file() {
        src="$1"
        dst="$2"
        log "Checking file: $dst"
        [ ! -e "$dst" ] && return 1
        cmp "$src" "$dst" &>/dev/null || return 1
        return 0
    }

    for file in spoutdxtoc.dll spout2pw.exe; do
        check_file "$spout2pw/spout2pw-dlls/x86_64-windows/$file" "$system32/$file" || return 1
    done

    log "Checking for service"
    if ! grep -q 'Services\\\\Spout2Pw' $wineprefix/system.reg; then
        log "Service is missing"
        return 1
    fi
}

prepare_prefix() {
    if check_spout2pw_install; then
        log "Spout2PW install is up-to-date"
        return
    fi

    show_info "Installing/updating Spout2PW into Wine prefix..."

    # Make sure spout2pw does not start up during the install, as it could lock the file.
    [ -e "$system32/spout2pw.exe" ] && rm -f "$system32/spout2pw.exe"
    
    run_in_prefix cmd /c "rundll32 setupapi.dll,InstallHinfSection DefaultInstall 128 Z:${spout2pw//\//\\}\\spout2pw.inf" || fatal "Installation failed"

    check_spout2pw_install || fatal "Installation unsuccessful"
    show_info "Installation successful"

}

prepare_proton() {
    if ! grep -q 'WINEDLLPATH.*in os.environ' "$protonpath/proton"; then
        fatal "This Proton version is too old to work with Spout2PW.\n\nSpout2PW requires a recent Proton 10."
    fi

    version="$(grep "^;; Version:" "$protonpath/files/share/wine/wine.inf" | cut -d: -f2 | sed -e 's/^ *//' -e 's/ *$//')"
    version="${version%%}"
    log "Wine version: '$version'"

    case "$version" in
        "Wine 10."*)
        ;;
        *)
            fatal "Unsupported Wine/Proton version: $version.\n\nSpout2PW currently requires Proton 10."
        ;;
    esac

}

setup_env() {
    export WINEDLLPATH="$spout2pw/spout2pw-dlls"
    if [ "$enable_debug" = 1 ]; then
        export PROTON_LOG=+spout2pw
        export PIPEWIRE_DEBUG='funnel*:5'
    fi
}

main() {
    quiet=0

    setup_logging

    log "Spout2PW install path: $spout2pw"

    enable_debug=0
    while [ "$#" -ge 1 ]; do
        case "$1" in
            -debug)
                enable_debug=1
                shift
                ;;
            -quiet)
                quiet=1
                shift
                ;;
            -*)
                fatal "Unknown argument: $1"
                ;;
            *)
                break
                ;;
        esac
    done

    check_environment
    check_pipewire
    find_gbm_backends

    verb="$1"

    case "$verb" in
        umu-run|/*/umu-run)
            setup_umu "$@"
        ;;
        wine|/*/wine|wine|/*/wine)
            setup_wine "$@"
        ;;
        */steam-launch-wrapper|*/proton)
            setup_steam "$@"
        ;;
        "")
            usage
            exit 1
        ;;
        *)
            fatal "Unknown command: $verb. spout2pw only supports Steam/Proton and umu-run."
    esac

    validate_paths
    prepare_proton
    prepare_prefix
    setup_env

    ret=0
    "$@" || ret="$?"
    log "Command exit status: $ret"
    exit $ret
}

main "$@"
ret="$?"
[ "$ret" != 0 ] && fatal "Unknown error $ret, see terminal log"

