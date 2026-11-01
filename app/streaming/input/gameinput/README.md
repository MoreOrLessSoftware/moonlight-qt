# GameInput (vendored)

From the `Microsoft.GameInput` NuGet package, version 4.0.302:

- `GameInput.h` = `native/include/v4/GameInput.h` (API version 4)
- `GameInputLoader.cpp` = `native/src/GameInput.cpp` (the static-library loader, renamed)

Both files are MIT licensed (see their headers). The loader locates the GameInput
redistributable (`GameInputRedist.dll`) at runtime; when it is not installed,
`GameInputCreate()` fails and Moonlight keeps using SDL for all gamepads.
The redistributable itself is not shipped with Moonlight.
