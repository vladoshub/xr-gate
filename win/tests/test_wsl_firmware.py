import gzip
import importlib.util
import json
import lzma
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location('wsl_firmware_test', Path(__file__).resolve().parents[1] / 'linux/wsl_firmware.py')
firmware = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(firmware)


class FirmwareTest(unittest.TestCase):
    def manager(self, root, settings=None):
        source = root / 'lib/firmware'
        source.mkdir(parents=True)
        parameter = root / 'sys/module/firmware_class/parameters/path'
        parameter.parent.mkdir(parents=True)
        parameter.write_text('')
        return firmware.Firmware(settings or {}, root=root)

    def fixture_command(self, manager, calls, mounted):
        def invoke(args, **kwargs):
            args = list(map(str, args))
            calls.append(args)
            code, text = 0, ''
            if args[0] == 'mountpoint': code = 0 if mounted[0] else 1
            elif args[:2] == ['mount', '--bind']: mounted[0] = True
            elif args[0] == 'dmesg': text = ''
            elif args[0] == 'dpkg-query': text = 'installed\ninstalled\n'
            return subprocess.CompletedProcess(args, code, stdout=text, stderr='')
        return invoke

    def test_boot_and_attach_restore_mount_and_kernel_path_without_a_new_download(self):
        with tempfile.TemporaryDirectory() as folder:
            manager = self.manager(Path(folder))
            calls, mounted = [], [False]
            with patch.object(manager, 'command', side_effect=self.fixture_command(manager,calls,mounted)), patch.object(firmware.os.path, 'samefile', return_value=True):
                manager.ensure_packages()
                manager.apply()
                self.assertEqual(manager.parameter.read_bytes(), str(manager.destination).encode())
                self.assertTrue(manager.changed)
                manager.changed = False
                manager.apply()
                self.assertFalse(manager.changed)
                self.assertEqual(sum(c[:2] == ['mount','--bind'] for c in calls), 1)
                manager.parameter.write_text(str(manager.destination)+'\n\n')
                manager.apply()
                self.assertEqual(manager.parameter.read_text(),str(manager.destination))
                # Simulate a new WSL VM: the permanent files remain but the
                # shared mount and firmware_class.path have disappeared.
                mounted[0] = False
                manager.parameter.write_text('')
                manager.apply()
                self.assertEqual(sum(c[:2] == ['mount','--bind'] for c in calls), 2)
                self.assertEqual(manager.parameter.read_text(), str(manager.destination))
                self.assertFalse(any(c[0] == 'apt-get' for c in calls))

    def test_missing_packages_are_installed_from_ubuntu_with_configurable_names(self):
        with tempfile.TemporaryDirectory() as folder:
            manager = self.manager(Path(folder), {'XR_BLUETOOTH_FIRMWARE_PACKAGES':'custom-firmware,zstd'})
            missing = subprocess.CompletedProcess([], 1, stdout='', stderr='not installed')
            done = subprocess.CompletedProcess([], 0, stdout='', stderr='')
            with patch.object(manager, 'command', side_effect=[missing,done,done]) as run:
                manager.ensure_packages()
                self.assertEqual(run.call_args_list[-1].args[0], ['apt-get','install','-y','--no-install-recommends','custom-firmware','zstd'])
            with self.assertRaises(ValueError): firmware.packages({'XR_BLUETOOTH_FIRMWARE_PACKAGES':'pkg;command'})

    def test_bluetooth_kernel_requests_decompress_generic_firmware_and_preserve_good_files_on_failure(self):
        with tempfile.TemporaryDirectory() as folder:
            manager = self.manager(Path(folder))
            target = manager.source / 'vendor_x/controller.bin'
            target.parent.mkdir()
            compressed = target.with_name(target.name+'.gz')
            with gzip.open(compressed,'wb') as handle: handle.write(b'firmware fixture')
            messages = ('[1] bluetooth hci7: Direct firmware load for vendor_x/controller.bin failed with error -2\n'
                        '[2] unrelated: Direct firmware load for gpu/missing.bin failed with error -2\n')
            with patch.object(manager,'command',return_value=subprocess.CompletedProcess([],0,stdout=messages,stderr='')):
                self.assertEqual(manager.requested_files(), ['vendor_x/controller.bin'])
                manager.materialize(manager.requested_files()[0])
            self.assertEqual(target.read_bytes(), b'firmware fixture')
            manager.repaired.clear()
            manager.materialize('vendor_x/controller.bin')
            self.assertEqual(manager.repaired, [])
            compressed.unlink()
            broken = target.with_name(target.name+'.xz')
            broken.write_bytes(b'invalid xz payload')
            os.utime(broken, ns=(target.stat().st_mtime_ns+1000000,)*2)
            with self.assertRaises(lzma.LZMAError): manager.materialize('vendor_x/controller.bin')
            self.assertEqual(target.read_bytes(), b'firmware fixture')
            self.assertFalse(list(target.parent.glob('.xr-gate-firmware-*')))

    def test_service_is_persistent_self_contained_and_idempotent(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            manager = self.manager(root, {'XR_WSL_FIRMWARE_FILES':'vendor_y/controller.bin'})
            calls = []
            with patch.object(manager,'command',side_effect=self.fixture_command(manager,calls,[True])):
                manager.install_service()
                manager.install_service()
            unit = root / 'etc/systemd/system' / firmware.SERVICE
            self.assertIn('Before=bluetooth.service',unit.read_text())
            self.assertIn('WantedBy=multi-user.target',unit.read_text())
            self.assertNotIn('/mnt/c',unit.read_text())
            helper = root / 'usr/local/lib/xr-gate-wsl/wsl_firmware.py'
            self.assertEqual(helper.read_bytes(),Path(firmware.__file__).read_bytes())
            self.assertEqual(json.loads((root/'etc/xr-gate-wsl/firmware.json').read_text())['XR_WSL_FIRMWARE_FILES'],'vendor_y/controller.bin')
            self.assertEqual(sum(c == ['systemctl','daemon-reload'] for c in calls),1)
            self.assertIn(['systemctl','enable',firmware.SERVICE],calls)
            disabled = firmware.Firmware({'XR_WSL_FIRMWARE':'0'},root=root)
            with patch.object(disabled,'command') as run:
                disabled.apply()
                run.assert_not_called()
                disabled.install_service()
                run.assert_called_once_with(['systemctl','disable',firmware.SERVICE])
            self.assertEqual(json.loads((root/'etc/xr-gate-wsl/firmware.json').read_text())['XR_WSL_FIRMWARE'],'0')

    def test_a_different_working_hci_does_not_mask_the_selected_dongle_failure(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            manager = self.manager(root)
            usb = root / 'sys/bus/usb/devices'
            for bus,identity,hci in [('1-2','1111:2222','hci1'),('1-3','3333:4444','hci0')]:
                device = usb / bus
                device.mkdir(parents=True)
                (device/'idVendor').write_text(identity.split(':')[0])
                (device/'idProduct').write_text(identity.split(':')[1])
                node = device / (bus+':1.0') / 'bluetooth' / hci
                node.mkdir(parents=True)
                alias = root/'sys/class/bluetooth'/hci
                alias.parent.mkdir(parents=True,exist_ok=True)
                alias.symlink_to(node)
            with patch.object(manager,'command',return_value=subprocess.CompletedProcess([],1,stdout='',stderr='unknown object')) as query:
                self.assertEqual(manager.selected_controller('1111:2222'),'')
                self.assertEqual(query.call_args.args[0][5],'/org/bluez/hci1')
            with patch.object(manager,'command',return_value=subprocess.CompletedProcess([],0,stdout='s "A0:11:22:33:44:55"\n',stderr='')):
                self.assertEqual(manager.selected_controller('1111:2222'),'A0:11:22:33:44:55')
            duplicate = usb/'1-4'
            duplicate.mkdir()
            (duplicate/'idVendor').write_text('1111')
            (duplicate/'idProduct').write_text('2222')
            with self.assertRaisesRegex(RuntimeError,'Several Bluetooth dongles'):
                manager.selected_controller('1111:2222')

    def test_firmware_paths_cannot_replace_shared_root_or_escape_source(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            manager = self.manager(root)
            for invalid in ['/','/mnt/wsl','/etc/firmware','/mnt/wsl/../elsewhere']:
                with self.subTest(path=invalid), self.assertRaises(ValueError):
                    firmware.Firmware({'XR_WSL_FIRMWARE_PATH':invalid},root=root)
            for name in ['../../etc/shadow','/etc/shadow','.']:
                with self.subTest(name=name), self.assertRaises(ValueError): manager.materialize(name)
            outside = root/'outside.bin.gz'
            outside.write_bytes(b'outside')
            (manager.source/'escape.bin.gz').symlink_to(outside)
            with self.assertRaises(ValueError): manager.materialize('escape.bin')


if __name__ == '__main__': unittest.main()
