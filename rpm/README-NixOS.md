# Build suse-monad RPM on NixOS

This bundle lets you build the openSUSE RPM from NixOS without installing RPM tooling system-wide.

## Option 1: flakes

```bash
cd suse-monad-rpm-bundle-nixos
nix develop -c ./build-rpm-nixos.sh
```

Or:

```bash
cd suse-monad-rpm-bundle-nixos
nix run .#build-rpm
```

## Option 2: classic nix-shell

```bash
cd suse-monad-rpm-bundle-nixos
nix-shell -p rpm gcc gnumake gnutar gzip coreutils findutils gawk gnugrep --run './build-rpm-nixos.sh'
```

## Result

Binary RPM:

```bash
~/rpmbuild/RPMS/x86_64/suse-monad-1.0.0-0.x86_64.rpm
```

Source RPM:

```bash
~/rpmbuild/SRPMS/suse-monad-1.0.0-0.src.rpm
```
