package artery.transfusion;

// simple consuming server for TransfusionServer
simple TransfusionLoopback
{
    parameters:
        @class(artery::TransfusionLoopback);
        int port;
}

