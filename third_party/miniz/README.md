# miniz 3.0.2 — inflate only

Upstream: https://github.com/richgel999/miniz/tree/3.0.2

miniz_tinfl.c, miniz_tinfl.h and miniz_common.h are unmodified upstream files. miniz.h and miniz_export.h are SerialCtl static integration shims: expose only tinfl, disable heap/stdIO/time helpers, select x64 little-endian with alignment-safe reads. UpdateInflate.cpp compiles the source as C++17. ZIP directory/path/size validation and extraction policy remain in UpdateArchive.cpp. No external DLL, system unzip program or UI framework is needed.

The MIT notice from the upstream source is reproduced in LICENSE and delivered as MINIZ-LICENSE.txt. Pinned hashes are in ../manifest.json.
