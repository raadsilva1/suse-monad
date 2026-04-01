# suse-monad RPM bundle

Contents:
- `suse-monad.spec` — RPM spec file
- `suse-monad.changes` — OBS-style changelog file
- `suse-monad-1.0.0.tar.gz` — source tarball used by rpmbuild
- `build-rpm.sh` — local build helper for openSUSE Tumbleweed

## Build on openSUSE Tumbleweed

```bash
chmod +x build-rpm.sh
./build-rpm.sh
```

## Install resulting RPM

```bash
sudo rpm -Uvh ~/rpmbuild/RPMS/x86_64/suse-monad-1.0.0-0.x86_64.rpm
```

Then adjust the packaged policy if needed:

```bash
sudoedit /etc/suse-monad/suse-monad.json
sudo suse-monad --dry-run --verbose
```
