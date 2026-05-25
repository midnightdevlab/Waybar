#!/usr/bin/env bash
# Build and install the Waybar fork as an Arch package (waybar-custom),
# replacing the official extra/waybar via pacman's conflicts/replaces.
#
# Run as your regular user. The script will sudo only where needed
# (cleanup of /usr/local debris, chown of root-owned build/, pacman -U).

set -euo pipefail

if [[ ${EUID} -eq 0 ]]; then
  echo "error: run as your regular user, not root — makepkg will sudo when needed" >&2
  exit 1
fi

REPO_ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
PKGDIR="${REPO_ROOT}/roby-arch-package"

confirm() {
  local prompt="$1" reply
  read -r -p "${prompt} [y/N] " reply
  [[ "${reply}" =~ ^[yY]$ ]]
}

cleanup_usr_local() {
  local paths=(
    /usr/local/bin/waybar
    /usr/local/share/man/man5/waybar*.5
    /usr/local/etc/xdg/waybar
    /usr/local/lib/systemd/user/waybar.service
    /usr/local/share/licenses/waybar
  )
  local found=()
  for p in "${paths[@]}"; do
    for m in $(compgen -G "${p}" 2>/dev/null || true); do
      found+=("${m}")
    done
  done

  if [[ ${#found[@]} -eq 0 ]]; then
    return
  fi

  echo "Stale Waybar files from the old local-install.sh found under /usr/local:"
  printf '  %s\n' "${found[@]}"
  if confirm "Remove them with sudo rm -rf?"; then
    sudo rm -rf -- "${found[@]}"
    echo "  removed."
  else
    echo "  skipped (you can re-run this script later to clean up)."
  fi
}

fix_build_ownership() {
  local build="${REPO_ROOT}/build"
  [[ -d "${build}" ]] || return 0
  if find "${build}" -not -user "${USER}" -print -quit 2>/dev/null | grep -q .; then
    echo "build/ contains root-owned files from a prior sudo build."
    if confirm "Fix with sudo chown -R ${USER}:${USER} build?"; then
      sudo chown -R "${USER}:${USER}" "${build}"
      echo "  fixed."
    else
      echo "  skipped — meson reconfigure will likely fail."
    fi
  fi
}

main() {
  cleanup_usr_local
  fix_build_ownership

  cd "${PKGDIR}"
  echo "Building waybar-custom via makepkg…"
  makepkg -fsi

  echo
  echo "Post-install check:"
  pacman -Qi waybar-custom | sed -n '1,8p'
  echo "  which waybar: $(command -v waybar || echo '<not found>')"
  echo "  waybar --version: $(waybar --version 2>/dev/null || echo '<failed>')"
}

main "$@"
