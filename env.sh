#!/usr/bin/env bash
# Kich hoat moi truong nRF Connect SDK v3.4.0 cho project ME54BS62.
# Dung:  source ./env.sh      (bash hoac zsh)

NCS_VERSION="v3.4.0"
NCS_ROOT="${NCS_ROOT:-$HOME/ncs}"
NCS_TOOLCHAIN="${NCS_TOOLCHAIN:-$NCS_ROOT/toolchains/fbf7391cab}"

if [ ! -d "$NCS_TOOLCHAIN" ]; then
    echo "[env] Khong tim thay toolchain: $NCS_TOOLCHAIN" >&2
    return 1 2>/dev/null || exit 1
fi

_tc="$NCS_TOOLCHAIN"
export PATH="$_tc/bin:$_tc/usr/bin:$_tc/usr/local/bin:$_tc/opt/bin:$_tc/opt/nanopb/generator-bin:$_tc/nrfutil/bin:$_tc/opt/zephyr-sdk/gnu/arm-zephyr-eabi/bin:$_tc/opt/zephyr-sdk/gnu/riscv64-zephyr-elf/bin:$PATH"
export LD_LIBRARY_PATH="$_tc/lib:$_tc/lib/x86_64-linux-gnu:$_tc/usr/local/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GIT_EXEC_PATH="$_tc/usr/local/libexec/git-core"
export GIT_TEMPLATE_DIR="$_tc/usr/local/share/git-core/templates"
export PYTHONHOME="$_tc/usr/local"
export PYTHONPATH="$_tc/usr/local/lib/python3.12:$_tc/usr/local/lib/python3.12/site-packages"
export PERL5LIB="$_tc/etc/perl:$_tc/usr/share/perl5:$_tc/usr/lib/x86_64-linux-gnu/perl/5.30.0:$_tc/usr/share/perl/5.30.0:$_tc/usr/lib/x86_64-linux-gnu/perl-base"
export PERLLIB="$_tc/usr/share/perl5:$_tc/usr/lib/x86_64-linux-gnu/perl/5.30.0:$_tc/usr/share/perl/5.30.0:$_tc/usr/lib/x86_64-linux-gnu/perl-base"
export TCL_LIBRARY="$_tc/usr/local/lib/tcl8.6"
export TK_LIBRARY="$_tc/usr/local/lib/tk8.6"
export NRFUTIL_HOME="$_tc/nrfutil/home"
export ZEPHYR_TOOLCHAIN_VARIANT="zephyr/gnu"
export ZEPHYR_SDK_INSTALL_DIR="$_tc/opt/zephyr-sdk"
export ZEPHYR_BASE="$NCS_ROOT/$NCS_VERSION/zephyr"
unset _tc

echo "[env] nRF Connect SDK $NCS_VERSION san sang  (ZEPHYR_BASE=$ZEPHYR_BASE)"
