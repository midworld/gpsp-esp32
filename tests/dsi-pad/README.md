# DSI pad crops

- Run `python3 tests/dsi-pad/run.py`.
- Compare production crop logic with full source rectangles.
- Check both rotations, all opacity levels, and full, partial or empty rows.
- Compare recolored tiles with freshly drawn tiles for every state transition.
- Use ASan and UBSan; assert source and destination bounds.
