using System;
using System.Collections.Generic;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;

namespace SerialCtl.Win7.Connections
{
    internal sealed class TcpTerminalConnection : ITerminalConnection
    {
        private readonly string _host;
        private readonly int _port;
        private readonly bool _telnet;
        private readonly SemaphoreSlim _writeLock = new SemaphoreSlim(1, 1);
        private TcpClient _client;
        private NetworkStream _stream;
        private CancellationTokenSource _cancellation;
        private Task _readerTask;

        public TcpTerminalConnection(string host, int port, bool telnet)
        {
            _host = host;
            _port = port;
            _telnet = telnet;
        }

        public event EventHandler<DataReceivedEventArgs> DataReceived;
        public event EventHandler<StatusEventArgs> StatusChanged;

        public bool IsConnected
        {
            get { return _client != null && _client.Connected; }
        }

        public async Task ConnectAsync()
        {
            if (IsConnected)
            {
                return;
            }

            TcpClient client = new TcpClient();
            client.NoDelay = true;
            await client.ConnectAsync(_host, _port).ConfigureAwait(false);

            _client = client;
            _stream = client.GetStream();
            _cancellation = new CancellationTokenSource();
            _readerTask = ReadLoopAsync(_cancellation.Token);
            RaiseStatus((_telnet ? "Telnet" : "TCP") + " 已连接：" + _host + ":" + _port, false);
        }

        public async Task DisconnectAsync()
        {
            CancellationTokenSource cancellation = _cancellation;
            _cancellation = null;
            if (cancellation != null)
            {
                cancellation.Cancel();
            }

            NetworkStream stream = _stream;
            _stream = null;
            if (stream != null)
            {
                stream.Close();
            }

            TcpClient client = _client;
            _client = null;
            if (client != null)
            {
                client.Close();
            }

            Task reader = _readerTask;
            _readerTask = null;
            if (reader != null)
            {
                try
                {
                    await reader.ConfigureAwait(false);
                }
                catch
                {
                    // The read normally ends with an exception when the socket closes.
                }
            }

            cancellation?.Dispose();
            RaiseStatus("网络连接已断开", false);
        }

        public async Task SendAsync(byte[] data)
        {
            NetworkStream stream = _stream;
            if (stream == null)
            {
                throw new InvalidOperationException("网络尚未连接。");
            }

            await _writeLock.WaitAsync().ConfigureAwait(false);
            try
            {
                await stream.WriteAsync(data, 0, data.Length).ConfigureAwait(false);
                await stream.FlushAsync().ConfigureAwait(false);
            }
            finally
            {
                _writeLock.Release();
            }
        }

        private async Task ReadLoopAsync(CancellationToken cancellationToken)
        {
            byte[] buffer = new byte[4096];
            try
            {
                while (!cancellationToken.IsCancellationRequested)
                {
                    NetworkStream stream = _stream;
                    if (stream == null)
                    {
                        break;
                    }

                    int read = await stream.ReadAsync(buffer, 0, buffer.Length, cancellationToken).ConfigureAwait(false);
                    if (read == 0)
                    {
                        RaiseStatus("远端已关闭连接", true);
                        break;
                    }

                    byte[] payload;
                    byte[] reply;
                    if (_telnet)
                    {
                        payload = TelnetCodec.Decode(buffer, read, out reply);
                        if (reply.Length > 0)
                        {
                            await SendAsync(reply).ConfigureAwait(false);
                        }
                    }
                    else
                    {
                        payload = new byte[read];
                        Buffer.BlockCopy(buffer, 0, payload, 0, read);
                    }

                    if (payload.Length > 0)
                    {
                        DataReceived?.Invoke(this, new DataReceivedEventArgs(payload));
                    }
                }
            }
            catch (OperationCanceledException)
            {
            }
            catch (Exception ex)
            {
                if (!cancellationToken.IsCancellationRequested)
                {
                    RaiseStatus("网络读取失败：" + ex.Message, true);
                }
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
            _writeLock.Dispose();
        }

        private static class TelnetCodec
        {
            private const byte Iac = 255;
            private const byte Do = 253;
            private const byte Dont = 254;
            private const byte Will = 251;
            private const byte Wont = 252;

            public static byte[] Decode(byte[] input, int count, out byte[] reply)
            {
                List<byte> output = new List<byte>(count);
                List<byte> response = new List<byte>();

                for (int index = 0; index < count; index++)
                {
                    byte current = input[index];
                    if (current != Iac)
                    {
                        output.Add(current);
                        continue;
                    }

                    if (index + 1 >= count)
                    {
                        break;
                    }

                    byte command = input[++index];
                    if (command == Iac)
                    {
                        output.Add(Iac);
                        continue;
                    }

                    if ((command == Do || command == Dont || command == Will || command == Wont) && index + 1 < count)
                    {
                        byte option = input[++index];
                        if (command == Do)
                        {
                            response.AddRange(new[] { Iac, Wont, option });
                        }
                        else if (command == Will)
                        {
                            response.AddRange(new[] { Iac, Dont, option });
                        }
                    }
                }

                reply = response.ToArray();
                return output.ToArray();
            }
        }
    }
}
