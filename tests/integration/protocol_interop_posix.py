"""Independent lrzsz binary interoperability; socketpair is not a physical UART."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("probe", "reference-dir", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    if os.name != "posix":
        raise SystemExit("Requires POSIX full-duplex socketpair")
    args.probe = args.probe.resolve()
    args.reference_dir = args.reference_dir.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    results = []
    report = {"status": "RUNNING", "cases": results, "scope": "Production C++ protocol engine and independent "
              "lrzsz executables over full-duplex socketpair; no UART, OS driver or GUI latency claim",
              "reference": {name: hashlib.sha256((args.reference_dir/name).read_bytes()).hexdigest()
                            for name in ("rx", "rb", "rz", "sx", "sb", "sz")}}
    try:
        for protocol in ("x", "y", "z"):
            for direction in ("send", "receive"):
                for size in (1, 128, 12345, 65539):
                    case = args.output / f"{protocol}-{direction}-{size}"
                    case.mkdir()
                    payload = bytes((i*173+i//7)&255 for i in range(size))
                    source, received = case/"fixture.bin", case/"received.bin"
                    source.write_bytes(payload)
                    reference_dir = case/"peer"
                    reference_dir.mkdir()
                    if direction == "send":
                        command = [str(args.reference_dir/{"x":"rx","y":"rb","z":"rz"}[protocol]), "-b", "-q"]
                        if protocol == "x":
                            command.append("fixture.bin")
                        actual = reference_dir/"fixture.bin"
                    else:
                        command = [str(args.reference_dir/{"x":"sx","y":"sb","z":"sz"}[protocol]), "-b", "-q", str(source)]
                        actual = received
                    left, right = socket.socketpair()
                    reference = probe = None
                    started = time.monotonic()
                    with (case/"reference.log").open("wb") as ref_log, (case/"probe.log").open("wb") as probe_log:
                        try:
                            reference = subprocess.Popen(command, stdin=right, stdout=right, stderr=ref_log,
                                                         cwd=reference_dir, close_fds=True)
                            probe = subprocess.Popen([str(args.probe), direction, protocol,
                                str(source if direction=="send" else received), str(size)],
                                stdin=left, stdout=left, stderr=probe_log, close_fds=True)
                            # The parent must not retain a peer endpoint: EOF
                            # needs to reach the other process on an early exit.
                            left.close()
                            right.close()
                            probe_code = probe.wait(timeout=40)
                            reference_code = reference.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            probe_code = reference_code = -1
                        finally:
                            left.close()
                            right.close()
                            for child in (probe, reference):
                                if child and child.poll() is None:
                                    child.kill()
                                    child.wait()
                    expected = payload + bytes([26])*((-size)%128) if protocol=="x" and direction=="send" else payload
                    valid = probe_code==reference_code==0 and actual.exists() and actual.read_bytes()==expected
                    result = {"protocol":protocol,"direction":direction,"size":size,"probe":probe_code,
                              "reference":reference_code,"exact_payload_and_protocol_padding":valid,
                              "padding_bytes":len(expected)-size,"seconds":round(time.monotonic()-started,3)}
                    results.append(result)
                    print(json.dumps(result), flush=True)
                    if not valid:
                        for name in ("probe.log", "reference.log"):
                            print(name + ": " + (case/name).read_text(errors="replace")[-6000:], flush=True)
                        raise RuntimeError("Interop failed; original peer/probe logs are retained: " + case.name)
        report["status"] = "PASS"
    except BaseException as exc:
        report.update(status="FAIL", error=str(exc))
        raise
    finally:
        (args.output/"interop-results.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    print("PASS: 24 independent reference cases; XMODEM's standard padding is explicitly accounted for")


if __name__ == "__main__":
    main()
