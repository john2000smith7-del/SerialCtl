using System;
using System.IO.Ports;
using System.Threading.Tasks;

namespace SerialCtl.Win7.Connections
{
    internal sealed class SerialTerminalConnection : ITerminalConnection
    {
        private readonly SerialSettings _settings;
        private readonly object _writeLock = new object();
        private SerialPort _port;

        public SerialTerminalConnection(SerialSettings settings)
        {
            _settings = settings;
        }

        public event EventHandler<DataReceivedEventArgs> DataReceived;
        public event EventHandler<StatusEventArgs> StatusChanged;

        public bool IsConnected
        {
            get { return _port != null && _port.IsOpen; }
        }

        public Task ConnectAsync()
        {
            return Task.Run(() =>
            {
                if (IsConnected)
                {
                    return;
                }

                SerialPort port = _settings.CreatePort();
                port.DataReceived += OnSerialDataReceived;
                port.ErrorReceived += OnSerialErrorReceived;

                try
                {
                    port.Open();
                    _port = port;
                    RaiseStatus("串口已连接：" + _settings.PortName, false);
                }
                catch
                {
                    port.Dispose();
                    throw;
                }
            });
        }

        public Task DisconnectAsync()
        {
            return Task.Run(() =>
            {
                SerialPort port = _port;
                _port = null;
                if (port == null)
                {
                    return;
                }

                port.DataReceived -= OnSerialDataReceived;
                port.ErrorReceived -= OnSerialErrorReceived;
                try
                {
                    if (port.IsOpen)
                    {
                        port.Close();
                    }
                }
                finally
                {
                    port.Dispose();
                    RaiseStatus("串口已断开", false);
                }
            });
        }

        public Task SendAsync(byte[] data)
        {
            return Task.Run(() =>
            {
                SerialPort port = _port;
                if (port == null || !port.IsOpen)
                {
                    throw new InvalidOperationException("串口尚未连接。");
                }

                lock (_writeLock)
                {
                    port.Write(data, 0, data.Length);
                }
            });
        }

        private void OnSerialDataReceived(object sender, SerialDataReceivedEventArgs e)
        {
            SerialPort port = _port;
            if (port == null || !port.IsOpen)
            {
                return;
            }

            try
            {
                int count = port.BytesToRead;
                if (count <= 0)
                {
                    return;
                }

                byte[] buffer = new byte[count];
                int read = port.Read(buffer, 0, buffer.Length);
                if (read != buffer.Length)
                {
                    Array.Resize(ref buffer, read);
                }

                DataReceived?.Invoke(this, new DataReceivedEventArgs(buffer));
            }
            catch (Exception ex)
            {
                RaiseStatus("读取串口失败：" + ex.Message, true);
            }
        }

        private void OnSerialErrorReceived(object sender, SerialErrorReceivedEventArgs e)
        {
            RaiseStatus("串口错误：" + e.EventType, true);
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
                // Closing the application should continue even if a device vanished.
            }
        }
    }
}
