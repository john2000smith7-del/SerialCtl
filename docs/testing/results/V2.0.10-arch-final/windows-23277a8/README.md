# Windows evidence for 23277a8

Source: `23277a862cee1be79138847b35c8f422e53e8125`.

- `comparison.json` is the exact JSON summary printed by [paired Windows run 40791932764](https://github.com/john2000smith7-del/SerialCtl/actions/runs/40791932764), job `123178998356`. Its [artifact 12861694494](https://github.com/john2000smith7-del/SerialCtl/actions/runs/40791932764/artifacts/12861694494) retains every per-process log and sample distribution. Each value is the median of three per-process metrics, not a pooled percentile.
- The other three JSON files are the exact named JSON notices from [native Windows run 40791936678](https://github.com/john2000smith7-del/SerialCtl/actions/runs/40791936678), job `123179016669`. The [full diagnostic artifact](https://github.com/john2000smith7-del/SerialCtl/actions/runs/40791936678/artifacts/12862761665) contains the original reports and test logs.

The files were transcribed from the job log JSON without numerical edits. The paired benchmark uses Windows Server 2022, Release x64, the accepted baseline `9d70d93`, and identical test drivers. Native serial, SSH and SFTP transports are controlled fixtures; these results do not measure physical UART, USB drivers, SSH/SFTP servers, monitor scanout or Windows 7 hardware. See the [performance report](../../../../audit/V2.0.10-PERFORMANCE-REGRESSION.md) for methods and limitations.
