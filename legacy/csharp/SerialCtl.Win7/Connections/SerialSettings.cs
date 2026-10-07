using System.IO.Ports;

namespace SerialCtl.Win7.Connections
{
    internal sealed class SerialSettings
    {
        public string PortName { get; set; }
        public int BaudRate { get; set; }
        public int DataBits { get; set; }
        public Parity Parity { get; set; }
        public StopBits StopBits { get; set; }
        public Handshake Handshake { get; set; }

        public SerialPort CreatePort()
        {
            return new SerialPort(PortName, BaudRate, Parity, DataBits, StopBits)
            {
                Handshake = Handshake,
                ReadTimeout = 1000,
                WriteTimeout = 1000,
                DtrEnable = false,
                RtsEnable = false
            };
        }
    }
}
