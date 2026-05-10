# Basecamp Skins — Example Manifests

Example skin configuration files for testing the detached sidebar prototype.

## Schema Reference

Each skin is a JSON manifest with these top-level keys:

| Key | Type | Required | Default | Description |
|-----|------|----------|---------|-------------|
| `name` | string | yes | — | Human-readable skin name (logged at startup) |
| `version` | string | yes | — | Semantic version of the skin format |
| `window.frameless` | bool | no | `true` | Remove OS titlebar/decorations on Linux |
| `window.titleBarHeight` | int | no | `32` | Height of custom TrafficLightsTitleBar in pixels |
| `sidebar.detached` | bool | no | `false` | Render sidebar as separate floating window |
| `sidebar.defaultWidth` | int | no | `60` | Sidebar width in pixels (both embedded and detached) |
| `sidebar.defaultPosition.x` | int | no | `-76` | X position for detached sidebar (screen coords) |
| `sidebar.defaultPosition.y` | int | no | `200` | Y position for detached sidebar (screen coords) |
| `theme.background` | string | no | `"#171717"` | Main window background color (hex) |
| `theme.surface` | string | no | `"#262626"` | Panel/card surface color (hex) |
| `theme.border` | string | no | `"#434343"` | Border/divider color (hex) |

## Missing Keys = Defaults

SkinConfig parses the JSON and uses defaults for any missing keys. This means you can create a minimal skin:

```json
{
  "name": "minimal",
  "version": "0.1.0"
}
```

This would give you a frameless window with embedded sidebar at default position, using default theme colors.

## Creating New Skins

1. Copy `default.json` or `detached-sidebar.json` as a starting point
2. Modify the values you want to change
3. Test with: `LogosBasecamp --skin /path/to/your-skin.json`
4. Check console output for parse errors (logged via `qDebug()`)

## Skin Design Tips

- **Positioning**: The sidebar window uses screen coordinates. Position it relative to your main window size and desired offset from the left edge.
- **Theme colors**: These are C++ layer colors (window chrome, MainContainer background). QML content views use `Logos.Theme` tokens separately — skin colors and QML themes are independent for now.
- **Frameless on Linux**: When `frameless: true`, the TrafficLightsTitleBar renders close/minimize/fullscreen buttons. Make sure your sidebar doesn't overlap the title bar area (it won't when detached, since they're separate windows).
