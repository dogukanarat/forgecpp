# Runtime tests of the forgecpp image (yocto/kas/qemuarm64.yml, testimage):
# the units are up, radio-controller registered with forge-hub, forgectl and
# the Python package work on the target.
from oeqa.core.decorator.depends import OETestDepends
from oeqa.runtime.case import OERuntimeTestCase
from oeqa.runtime.decorator.package import OEHasPackage


def until(cmd, seconds=120):
    """Shell snippet: run `cmd` until it succeeds or `seconds` pass."""
    return "for i in $(seq %d); do (%s) && exit 0; sleep 1; done; exit 1" % (seconds, cmd)


class ForgecppTest(OERuntimeTestCase):

    @OETestDepends(['ssh.SSHTest.test_ssh'])
    @OEHasPackage(['forgecpp-hub', 'radio-controller'])
    def test_units_active(self):
        for unit in ('forge-hub', 'radio-controller', 'spectrum-analyzer'):
            status, out = self.target.run(until('systemctl is-active -q %s' % unit))
            if status != 0:
                _, out = self.target.run('systemctl status --no-pager %s; journalctl --no-pager -n 50 -u %s' % (unit, unit))
            self.assertEqual(status, 0, msg='%s is not active:\n%s' % (unit, out))

    @OETestDepends(['forgecpp.ForgecppTest.test_units_active'])
    def test_hub_lists_applications(self):
        status, out = self.target.run(
            until('curl -fs http://127.0.0.1/api/apps | grep -q radio-controller && '
                  'curl -fs http://127.0.0.1/api/apps | grep -q spectrum-analyzer'))
        if status != 0:
            _, out = self.target.run('curl -s http://127.0.0.1/api/apps')
        self.assertEqual(status, 0, msg='forge-hub does not list the applications:\n%s' % out)

    @OETestDepends(['forgecpp.ForgecppTest.test_units_active'])
    @OEHasPackage(['forgecpp-forgectl'])
    def test_forgectl(self):
        status, out = self.target.run(until('forgectl -e tcp://127.0.0.1:5557 get rx_lo_mhz', 60))
        self.assertEqual(status, 0, msg=out)
        self.assertIn('2412', out)

    @OEHasPackage(['python3-forge'])
    def test_python_package(self):
        status, out = self.target.run('python3 -c "from forge import c2, framing, messages_pb2; print(messages_pb2.info)"')
        self.assertEqual(status, 0, msg=out)
