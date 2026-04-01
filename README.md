# suse-monad

**suse-monad** is a JSON-driven desktop provisioner for **openSUSE Tumbleweed**, designed to install and configure a practical, ready-to-use **xMonad** environment with a clean, 
honest deployment model.

It focuses on simplicity, portability, and runtime flexibility by moving desktop behavior and package policy into a JSON configuration file instead of hardcoding those decisions 
into the binary.

---

## Overview

`suse-monad` provisions an **xMonad-on-X11** desktop for openSUSE Tumbleweed and reads its runtime policy from:

```text
/etc/suse-monad/suse-monad.json
```

This makes it possible to adjust installation behavior, package selection, and desktop policy without recompiling the application.

The project is explicit about the graphics stack:

- **xMonad is installed and configured as an X11 session/window manager**
- **Wayland is not misrepresented as the native xMonad session**
- **Wayland-friendly applications may still be installed through JSON policy when desired**

The result is a practical, user-oriented desktop setup centered on **xMonad running on X11**.
