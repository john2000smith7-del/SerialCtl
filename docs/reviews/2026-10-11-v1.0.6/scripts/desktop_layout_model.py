"""Read-only calculations of existing Win32 layout formulas, not GUI execution.

Run: python scripts/desktop_layout_model.py > evidence/desktop/layout-model.json
Source: MainWindow.cpp LoadUiState/ScaleUiMetrics/SaveUiState, LayoutCommandDialog.
This substitutes arithmetic only; it does not prove font, screenshot or DPI behavior.
"""
import json

rows = []
for dpi in (96, 120, 144, 192):
    scaled_maximum = round(650 * dpi / 96)
    stored = scaled_maximum
    loaded_before_metrics = max(260, min(650, stored))
    visible_after_metrics = max(round(260 * dpi / 96), loaded_before_metrics)
    rows.append({"dpi": dpi, "saved_physical_width": stored,
                 "loaded_before_ScaleUiMetrics": loaded_before_metrics,
                 "restored_physical_width_assuming_space_available": visible_after_metrics,
                 "lost_physical_width": stored - visible_after_metrics})
# MainWindow.cpp: StepTop 58, StepStride 18, labels/fields/footer below rows.
steps = []
for n in (1, 2, 10):
    add_y = 58 + n * 18
    error_y = add_y + 22 + 12 + 18 if n > 1 else add_y + 22
    steps.append({"steps": n, "client_height_DLU": error_y + 12 + 26,
                  "work_area_height_cap": False, "scrolling_container": False})
print(json.dumps({"kind": "source-formula reproduction; not actual Windows or DPI run",
                  "persisted_sidebar_width": rows, "command_dialog": steps}, indent=2))
