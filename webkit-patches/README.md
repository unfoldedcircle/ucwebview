# Patches for WPE WebKit

Optional patches for WPE WebKit that the library and the launcher work without. Each is meant for upstream
(<https://bugs.webkit.org>, component WPE WebKit) and carries its own description and sign-off. A patch is licensed
like the WebKit files it changes, not under this repository's MIT license: the files of the patch below
(`WebKeyboardEvent.h`, `WebEventFactoryWPE.cpp`, `WebKeyboardEventWPE.cpp`) are BSD-2-Clause.

| Patch | Against | What it does |
|-------|---------|--------------|
| [0001-WPE-Key-values-for-ChannelUp-ChannelDown-MediaPlayPause-and-Exit.patch](0001-WPE-Key-values-for-ChannelUp-ChannelDown-MediaPlayPause-and-Exit.patch) | WPE WebKit 2.54.2 | `KeyboardEvent.key` and `keyCode` for the remote control keys ChannelUp, ChannelDown (XF86ChannelUp/Down), MediaPlayPause (XF86AudioPlay on KEY_PLAYPAUSE) and Exit (XF86Close on KEY_EXIT), and the key values of `ISO_Left_Tab` (Tab) and `XF86Reload` (BrowserRefresh). Without it those keys arrive as "Unidentified", "MediaPlay" or "Close". The key table of [lib/README.md](../lib/README.md#key-names-dom-key-code-and-keycode) shows both cases |

Apply with `patch -p1` (or `git am`) in the top directory of the release tarball before building WebKit:

```shell
tar -xf wpewebkit-2.54.2.tar.xz && cd wpewebkit-2.54.2
patch -p1 < ../webkit-patches/0001-WPE-Key-values-for-ChannelUp-ChannelDown-MediaPlayPause-and-Exit.patch
```
