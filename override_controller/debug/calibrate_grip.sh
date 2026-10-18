#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CALIBRATION_PYTHON="${CALIBRATION_PYTHON:-python3}"
if ! "$CALIBRATION_PYTHON" -c 'import numpy; import scipy' >/dev/null 2>&1; then
  echo "Для мастера нужны NumPy/SciPy. Установи зависимости в отдельное окружение:"
  echo "  python3 -m venv ~/.venvs/controller-grip"
  echo "  ~/.venvs/controller-grip/bin/python -m pip install -r \"$SCRIPT_DIR/calibration_requirements.txt\""
  echo "  CALIBRATION_PYTHON=~/.venvs/controller-grip/bin/python bash \"$SCRIPT_DIR/calibrate_grip.sh\""
  exit 2
fi
exec "$CALIBRATION_PYTHON" "$SCRIPT_DIR/calibrate_optical_grip.py" "$@"
