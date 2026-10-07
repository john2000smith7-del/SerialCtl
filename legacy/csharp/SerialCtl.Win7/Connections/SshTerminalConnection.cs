using System;
using System.Globalization;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Renci.SshNet;

namespace SerialCtl.Win7.Connections
{
    internal sealed class SshTerminalConnection : ITerminalConnection
    {
        private readonly string _host;
        private readonly int _port;
        private readonly string _username;
        private readonly string _password;
        private readonly object _writeLock = new object();
        private SshClient _client;
        private ShellStream _shell;

        public SshTerminalConnection(string host, int port, string username, string password)
        {
            _host = host;
            _port = port;
            _username = username;
            _password = password;
        }

        public event EventHandler<DataReceivedEventArgs> DataReceived;
        public event EventHandler<StatusEventArgs> StatusChanged;

        public bool IsConnected
        {
            get { return _client != null && _client.IsConnected; }
        }

        public Task ConnectAsync()
        {
            return Task.Run(() =>
            {
                SshClient client = new SshClient(_host, _port, _username, _password);
                client.KeepAliveInterval = TimeSpan.FromSeconds(20);
                client.HostKeyReceived += (sender, args) =>
                {
                    // MVP behavior: show the fingerprint and trust it for this session.
                    // A later version will persist a known-hosts list.
                    string fingerprint = BitConverter.ToString(args.FingerPrint).Replace("-", ":");
                    RaiseStatus("SSH 主机指纹（本次会话信任）：" + fingerprint, false);
                    args.CanTrust = true;
                };

                try
                {
                    client.Connect();
                    ShellStream shell = client.CreateShellStream("xterm", 120, 40, 1024, 768, 4096);
                    shell.DataReceived += OnShellDataReceived;
                    _client = client;
                    _shell = shell;
                    RaiseStatus("SSH 已连接：" + _host + ":" + _port, false);
                }
                catch
                {
                    client.Dispose();
                    throw;
                }
            });
        }

        public Task DisconnectAsync()
        {
            return Task.Run(() =>
            {
                ShellStream shell = _shell;
                _shell = null;
                if (shell != null)
                {
                    shell.DataReceived -= OnShellDataReceived;
                    shell.Dispose();
                }

                SshClient client = _client;
                _client = null;
                if (client != null)
                {
                    try
                    {
                        if (client.IsConnected)
                        {
                            client.Disconnect();
                        }
                    }
                    finally
                    {
                        client.Dispose();
                    }
                }

                RaiseStatus("SSH 已断开", false);
            });
        }

        public Task SendAsync(byte[] data)
        {
            return Task.Run(() =>
            {
                ShellStream shell = _shell;
                if (shell == null || !IsConnected)
                {
                    throw new InvalidOperationException("SSH 尚未连接。");
                }

                lock (_writeLock)
                {
                    shell.Write(data, 0, data.Length);
                    shell.Flush();
                }
            });
        }

        private void OnShellDataReceived(object sender, Renci.SshNet.Common.ShellDataEventArgs e)
        {
            if (e.Data != null && e.Data.Length > 0)
            {
                DataReceived?.Invoke(this, new DataReceivedEventArgs(e.Data));
            }
        }

        private void RaiseStatus(string message, bool isError)
        {
            StatusChanged?.Invoke(this, new StatusEventArgs(message, isError));
        }

        public void Dispose()
        {
            try
            {
                DisconnectAsync().GetAwaiter().GetResult();
            }
            catch
            {
            }
        }
    }
}
