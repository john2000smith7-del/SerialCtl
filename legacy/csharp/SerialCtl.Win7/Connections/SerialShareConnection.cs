using System;
using System.Collections.Generic;
using System.IO.Ports;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;

namespace SerialCtl.Win7.Connections
{
    internal sealed class SerialShareConnection : ITerminalConnection
    {
        private readonly SerialSettings _settings;
        private readonly int _listenPort;
        private readonly object _clientsLock = new object();
        private readonly List<TcpClient> _clients = new List<TcpClient>();
        private readonly SemaphoreSlim _serialWriteLock = new SemaphoreSlim(1, 1);
        private readonly SemaphoreSlim _broadcastLock = new SemaphoreSlim(1, 1);
        private SerialPort _serialPort;
        private TcpListener _listener;
        private CancellationTokenSource _cancellation;
        private Task _acceptTask;

        public SerialShareConnection(SerialSettings settings, int listenPort)
        {
            _settings = settings;
            _listenPort = listenPort;
        }

        public event EventHandler<DataReceivedEventArgs> DataReceived;
        public event EventHandler<StatusEventArgs> StatusChanged;

        public bool IsConnected
        {
            get { return _serialPort != null && _serialPort.IsOpen && _listener != null; }
        }

        public Task ConnectAsync()
        {
            return Task.Run(() =>
            {
                if (IsConnected)
                {
                    return;
                }

                SerialPort serialPort = _settings.CreatePort();
                TcpListener listener = new TcpListener(IPAddress.Any, _listenPort);
                CancellationTokenSource cancellation = new CancellationTokenSource();

                try
                {
                    serialPort.DataReceived += OnSerialDataReceived;
                    serialPort.ErrorReceived += OnSerialErrorReceived;
                    serialPort.Open();
                    listener.Start();

                    _serialPort = serialPort;
                    _listener = listener;
                    _cancellation = cancellation;
                    _acceptTask = AcceptLoopAsync(cancellation.Token);
                    RaiseStatus("正在共享 " + _settings.PortName + "，监听 TCP 端口 " + _listenPort, false);
                }
                catch
                {
                    serialPort.DataReceived -= OnSerialDataReceived;
                    serialPort.ErrorReceived -= OnSerialErrorReceived;
                    if (serialPort.IsOpen)
                    {
                        serialPort.Close();
                    }
                    serialPort.Dispose();
                    listener.Stop();
                    cancellation.Dispose();
                    throw;
                }
            });
        }

        public async Task DisconnectAsync()
        {
            CancellationTokenSource cancellation = _cancellation;
            _cancellation = null;
            cancellation?.Cancel();

            TcpListener listener = _listener;
            _listener = null;
            listener?.Stop();

            List<TcpClient> clients;
            lock (_clientsLock)
            {
                clients = new List<TcpClient>(_clients);
                _clients.Clear();
            }
            foreach (TcpClient client in clients)
            {
                client.Close();
            }

            Task acceptTask = _acceptTask;
            _acceptTask = null;
            if (acceptTask != null)
            {
                try
                {
                    await acceptTask.ConfigureAwait(false);
                }
                catch
                {
                }
            }

            SerialPort serialPort = _serialPort;
            _serialPort = null;
            if (serialPort != null)
            {
                serialPort.DataReceived -= OnSerialDataReceived;
                serialPort.ErrorReceived -= OnSerialErrorReceived;
                try
                {
                    if (serialPort.IsOpen)
                    {
                        serialPort.Close();
                    }
                }
                finally
                {
                    serialPort.Dispose();
                }
            }

            cancellation?.Dispose();
            RaiseStatus("串口共享已停止", false);
        }

        public async Task SendAsync(byte[] data)
        {
            await WriteSerialAsync(data).ConfigureAwait(false);
        }

        private async Task AcceptLoopAsync(CancellationToken cancellationToken)
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                try
                {
                    TcpClient client = await _listener.AcceptTcpClientAsync().ConfigureAwait(false);
                    client.NoDelay = true;
                    lock (_clientsLock)
                    {
                        _clients.Add(client);
                    }
                    RaiseStatus("远程客户端已连接，当前 " + GetClientCount() + " 个", false);
                    _ = Task.Run(() => ReadClientLoopAsync(client, cancellationToken));
                }
                catch (ObjectDisposedException)
                {
                    break;
                }
                catch (SocketException ex)
                {
                    if (!cancellationToken.IsCancellationRequested)
                    {
                        RaiseStatus("接受客户端失败：" + ex.Message, true);
                    }
                    break;
                }
            }
        }

        private async Task ReadClientLoopAsync(TcpClient client, CancellationToken cancellationToken)
        {
            byte[] buffer = new byte[4096];
            try
            {
                NetworkStream stream = client.GetStream();
                while (!cancellationToken.IsCancellationRequested)
                {
                    int read = await stream.ReadAsync(buffer, 0, buffer.Length, cancellationToken).ConfigureAwait(false);
                    if (read == 0)
                    {
                        break;
                    }

                    byte[] data = new byte[read];
                    Buffer.BlockCopy(buffer, 0, data, 0, read);
                    await WriteSerialAsync(data).ConfigureAwait(false);
                }
            }
            catch (OperationCanceledException)
            {
            }
            catch (Exception ex)
            {
                if (!cancellationToken.IsCancellationRequested)
                {
                    RaiseStatus("远程客户端通信失败：" + ex.Message, true);
                }
            }
            finally
            {
                lock (_clientsLock)
                {
                    _clients.Remove(client);
                }
                client.Close();
                RaiseStatus("远程客户端已断开，当前 " + GetClientCount() + " 个", false);
            }
        }

        private void OnSerialDataReceived(object sender, SerialDataReceivedEventArgs e)
        {
            SerialPort serialPort = _serialPort;
            if (serialPort == null || !serialPort.IsOpen)
            {
                return;
            }

            try
            {
                int count = serialPort.BytesToRead;
                if (count <= 0)
                {
                    return;
                }
                byte[] buffer = new byte[count];
                int read = serialPort.Read(buffer, 0, buffer.Length);
                if (read != buffer.Length)
                {
                    Array.Resize(ref buffer, read);
                }

                DataReceived?.Invoke(this, new DataReceivedEventArgs(buffer));
                Task.Run(() => BroadcastAsync(buffer));
            }
            catch (Exception ex)
            {
                RaiseStatus("读取串口失败：" + ex.Message, true);
            }
        }

        private async Task BroadcastAsync(byte[] data)
        {
            await _broadcastLock.WaitAsync().ConfigureAwait(false);
            try
            {
                List<TcpClient> snapshot;
                lock (_clientsLock)
                {
                    snapshot = new List<TcpClient>(_clients);
                }

                foreach (TcpClient client in snapshot)
                {
                    try
                    {
                        NetworkStream stream = client.GetStream();
                        await stream.WriteAsync(data, 0, data.Length).ConfigureAwait(false);
                        await stream.FlushAsync().ConfigureAwait(false);
                    }
                    catch
                    {
                        lock (_clientsLock)
                        {
                            _clients.Remove(client);
                        }
                        client.Close();
                    }
                }
            }
            finally
            {
                _broadcastLock.Release();
            }
        }

        private async Task WriteSerialAsync(byte[] data)
        {
            SerialPort serialPort = _serialPort;
            if (serialPort == null || !serialPort.IsOpen)
            {
                throw new InvalidOperationException("串口尚未打开。");
            }

            await _serialWriteLock.WaitAsync().ConfigureAwait(false);
            try
            {
                await Task.Run(() => serialPort.Write(data, 0, data.Length)).ConfigureAwait(false);
            }
            finally
            {
                _serialWriteLock.Release();
            }
        }

        private void OnSerialErrorReceived(object sender, SerialErrorReceivedEventArgs e)
        {
            RaiseStatus("串口错误：" + e.EventType, true);
        }

        private int GetClientCount()
        {
            lock (_clientsLock)
            {
                return _clients.Count;
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
            _serialWriteLock.Dispose();
            _broadcastLock.Dispose();
        }
    }
}
