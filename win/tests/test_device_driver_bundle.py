import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from zipfile import ZipFile

WIN = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('device_windows_packager', WIN / 'package_windows.py')
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)


class DeviceBundleTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.drivers = self.root / 'drivers'
        self.launcher = self.root / 'XR-Gate.exe'
        self.launcher.write_bytes(b'MZ' + b'fixture' * 20)
        self.archive = self.root / 'windows.zip'

    def driver(self, device):
        package = self.drivers / device / '60Hz-extended_sbs/xr_tracking'
        for name in ('driver.vrdrivermanifest', 'bin/win64/driver_xr_tracking.dll', 'bin/win64/openvr_api.dll',
                     'LICENSE', 'THIRD_PARTY_NOTICES.md'):
            path = package / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('fixture')
        settings = package / 'resources/settings/default.vrsettings'
        settings.parent.mkdir(parents=True)
        settings.write_text(json.dumps({'xr_tracking': {
            'deviceProfile': device, 'displayFrequency': 60, 'isDisplayOnDesktop': True,
            'runtimePoseTransport': 'tcp', 'runtimeHandTransport': 'tcp',
            'runtimeControllerStateTransport': 'tcp'}}))
        return package

    def build(self, devices=None):
        packager.build(WIN, self.drivers, self.archive, '60', 'extended_sbs', launcher=self.launcher, devices=devices)
        return packager.verify(self.archive, '60', 'extended_sbs', devices=devices)

    def test_same_frequency_devices_have_separate_packages(self):
        self.driver('alpha')
        self.driver('beta')
        manifest = self.build()
        self.assertEqual(manifest['schema'], 2)
        self.assertEqual(manifest['devices'], ['alpha', 'beta'])
        self.assertEqual(len(manifest['variants']), 2)
        with ZipFile(self.archive) as archive:
            for device in ('alpha', 'beta'):
                relative = packager.variant_path(60, 'extended_sbs', device)
                self.assertIn(f'{packager.ROOT_NAME}/{relative}/bin/win64/driver_xr_tracking.dll', archive.namelist())

    def test_missing_device_never_reuses_another_package(self):
        self.driver('alpha')
        with self.assertRaises(FileNotFoundError):
            self.build('alpha beta')

    def test_settings_must_match_the_folder(self):
        package = self.driver('alpha')
        settings = package / 'resources/settings/default.vrsettings'
        data = json.loads(settings.read_text())
        data['xr_tracking']['deviceProfile'] = 'beta'
        settings.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'does not match its folder'):
            self.build()

    def test_verifier_checks_the_requested_devices(self):
        self.driver('alpha')
        self.build()
        with self.assertRaisesRegex(ValueError, 'devices do not match'):
            packager.verify(self.archive, '60', 'extended_sbs', devices='beta')


if __name__ == '__main__':
    unittest.main()
