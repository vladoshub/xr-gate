import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('driver_profiles', ROOT / 'drivers/device_profiles.py')
profiles = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profiles)


class DeviceProfilesTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.write('drivers/openvr_driver/resources/settings/default.vrsettings', '{"xr_tracking":{}}')
        for name in ('alpha', 'beta'):
            self.write(f'drivers/openvr_driver/devices/{name}/settings/default.vrsettings', '{"xr_tracking":{}}')
            self.write(f'drivers/monado_driver/profiles/{name}.env', 'export XR_MONADO_EYE_WIDTH=1200\n')
        self.write('drivers/monado_driver/profiles/runtime_data.env', 'export XR_MONADO_TCP_PORT=45810\n')
        self.write('drivers/monado_driver/profiles/controller_input.env', 'export XR_MONADO_INPUT=touch\n')

    def write(self, relative, content):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return path

    def test_discovery_excludes_shared_monado_options(self):
        self.assertEqual(profiles.configured_devices(self.root, 'steamvr'), ['alpha', 'beta', 'generic'])
        self.assertEqual(profiles.configured_devices(self.root, 'monado'), ['alpha', 'beta'])

    def test_selection_does_not_fall_back_to_another_device(self):
        self.assertEqual(profiles.select_devices(self.root, 'steamvr', 'beta,alpha beta'), ['beta', 'alpha'])
        with self.assertRaisesRegex(ValueError, 'No steamvr configuration for missing'):
            profiles.select_devices(self.root, 'steamvr', 'missing')
        for name in ('../alpha', '/alpha', 'alpha.', ''):
            with self.subTest(name=name), self.assertRaises(ValueError):
                profiles.device_id(name)

    def prepare_matrix(self):
        for name in ('device_profiles.py', 'build_profiles.sh'):
            self.write('drivers/' + name, (ROOT / 'drivers' / name).read_text())
        self.write('drivers/openvr_driver/scripts/openvr_display_common.sh',
                   (ROOT / 'drivers/openvr_driver/scripts/openvr_display_common.sh').read_text())
        stub = """#!/usr/bin/env bash
python3 - <<'PY'
import json, os
keys = ('XR_OPENVR_DEVICE', 'XR_MONADO_DEVICE', 'INSTALL_DRIVER_PACKAGE', 'BIN_DIR', 'BUILD_DIR')
print(json.dumps({key: os.environ.get(key, '') for key in keys}))
PY
"""
        self.write('drivers/openvr_driver/scripts/build_driver.sh', stub)
        self.write('drivers/monado_driver/scripts/linux/install.sh', stub)
        return dict(os.environ, ROOT_PROJECT=str(self.root), XR_ROOT_PROJECT=str(self.root),
                    XR_BIN_ROOT=str(self.root / 'bin'), XR_BUILD_ROOT=str(self.root / 'build'),
                    XR_OPENVR_DEVICES='alpha beta', XR_MONADO_DEVICES='alpha beta',
                    XR_OPENVR_BUILD_FREQUENCIES='60 90', XR_OPENVR_BUILD_MODES='direct',
                    INSTALL_DRIVERS_ROOT=str(self.root / 'bin/drivers'))

    def run_matrix(self, driver, env):
        return subprocess.run(['bash', self.root / 'drivers/build_profiles.sh', driver],
                              env=env, capture_output=True, text=True, timeout=20)

    @unittest.skipUnless(shutil.which('bash'), 'Bash is required')
    def test_steamvr_keeps_device_builds_and_outputs_separate(self):
        result = self.run_matrix('steamvr', self.prepare_matrix())
        self.assertEqual(result.returncode, 0, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(len(records), 4)
        self.assertEqual({row['XR_OPENVR_DEVICE'] for row in records}, {'alpha', 'beta'})
        self.assertEqual(len({row['BUILD_DIR'] for row in records}), 4)
        for row in records:
            relative = Path(row['INSTALL_DRIVER_PACKAGE']).relative_to(self.root / 'bin/drivers')
            self.assertEqual(relative.parts[0], row['XR_OPENVR_DEVICE'])
            self.assertEqual(relative.parts[-1], 'xr_tracking')

    @unittest.skipUnless(shutil.which('bash'), 'Bash is required')
    def test_monado_separates_packages_and_reuses_compilation(self):
        result = self.run_matrix('monado', self.prepare_matrix())
        self.assertEqual(result.returncode, 0, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(len(records), 2)
        self.assertEqual(len({row['BUILD_DIR'] for row in records}), 1)
        self.assertEqual({row['BIN_DIR'] for row in records},
                         {str(self.root / f'bin/drivers/{name}/monado_driver') for name in ('alpha', 'beta')})

    @unittest.skipUnless(shutil.which('bash'), 'Bash is required')
    def test_invalid_profile_stops_before_a_builder_runs(self):
        env = self.prepare_matrix()
        env['XR_OPENVR_DEVICES'] = 'missing'
        result = self.run_matrix('steamvr', env)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, '')
        self.assertIn('No steamvr configuration for missing', result.stderr)

    @unittest.skipUnless(shutil.which('bash'), 'Bash is required')
    def test_relocated_monado_uses_its_own_device_and_display(self):
        (self.root / 'shared/include').mkdir(parents=True)
        package = self.root / 'out/runtime'
        driver = package / 'bin/drivers/alpha/monado_driver'
        driver.mkdir(parents=True)
        (driver / 'start.sh').write_text((ROOT / 'drivers/monado_driver/scripts/linux/start.sh').read_text())
        (driver / 'device-profile').write_text('alpha\n')
        (driver / 'profiles').mkdir()
        (driver / 'profiles/alpha.env').write_text('export XR_MONADO_EYE_WIDTH="${XR_MONADO_EYE_WIDTH:-1200}"\n')
        (driver / 'configs/display').mkdir(parents=True)
        (driver / 'configs/display/default.yaml').write_text('1920')
        (driver / 'display_optics_config.py').write_text(
            'from pathlib import Path\nimport sys\n'
            'print("XR_MONADO_EYE_WIDTH=" + Path(sys.argv[sys.argv.index("--config")+1]).read_text())\n')
        service = driver / 'monado-service'
        service.write_text("""#!/usr/bin/env bash
python3 - <<'PY'
import json, os
print('RESULT=' + json.dumps({key: os.environ.get(key) for key in
    ('XR_MONADO_DEVICE', 'XR_MONADO_EYE_WIDTH', 'XR_TRACKING_ROOT')}))
PY
""")
        service.chmod(0o755)
        moved = self.root / 'relocated'
        shutil.move(str(package), moved)
        driver = moved / 'bin/drivers/alpha/monado_driver'
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(('XR_', 'XRT_', 'MONADO_')) and key not in ('ROOT_PROJECT', 'BIN_DIR', 'BUILD_DIR')}

        def start(extra_env=None):
            result = subprocess.run(['bash', driver / 'start.sh'], env=dict(env, **(extra_env or {})),
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return json.loads(next(line[7:] for line in result.stdout.splitlines() if line.startswith('RESULT=')))

        self.assertEqual(start(), {'XR_MONADO_DEVICE': 'alpha', 'XR_MONADO_EYE_WIDTH': '1200',
                                   'XR_TRACKING_ROOT': str(moved)})
        self.assertEqual(start({'XR_MONADO_EYE_WIDTH': '999'})['XR_MONADO_EYE_WIDTH'], '999')
        custom = driver / 'custom.yaml'
        custom.write_text('800')
        self.assertEqual(start({'XR_MONADO_DISPLAY_CONFIG': str(custom)})['XR_MONADO_EYE_WIDTH'], '800')
        (driver / 'configs/display/alpha.yaml').write_text('600')
        self.assertEqual(start()['XR_MONADO_EYE_WIDTH'], '600')


if __name__ == '__main__':
    unittest.main()
