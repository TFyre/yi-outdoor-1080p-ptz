/*
 * onvif - minimal ONVIF Profile S server for the YI Outdoor 1080p PTZ.
 *
 * Hand-rolled SOAP (no gSOAP - far too heavy for this device): a small
 * socket loop answers the ~10 methods real clients use (ONVIF Device
 * Manager, Blue Iris, Home Assistant):
 *
 *   Device service: GetDeviceInformation, GetCapabilities, GetScopes,
 *                   GetSystemDateAndTime
 *   Media service:  GetProfiles, GetStreamUri  (advertises the RTSP
 *                   stream + the AAC audio track)
 *   PTZ service:    ContinuousMove, Stop, SetPreset, GotoPreset,
 *                   GetPresets, GetStatus
 *
 * PTZ maps 1:1 onto the proven mqueue envelopes (src/ptz/ptz.c, the
 * app-family IPC protocol): ContinuousMove's velocity sign picks the
 * direction; the motor runs until Stop (or a 10 s in-daemon cap if the
 * client vanishes mid-move). Requests are parsed with targeted string
 * extraction - ONVIF client messages are machine-generated and
 * predictable; full XML parsing buys nothing.
 *
 * usage: onvif [-p PORT]   (default 8082; no WS-Discovery yet - add
 * the device manually in ODM as http://IP:PORT/onvif/device_service)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <mqueue.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int g_port = 8082;
static int g_mq_ok = 1;      /* mqueue send failures only logged once */

/* ---- the ptz envelopes (src/ptz/ptz.c, app-family IPC) ---- */
static const char *g_mq = "/ipc_dispatch";

static void put32(unsigned char *p, unsigned v)
{
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

static void put16(unsigned char *p, unsigned v)
{
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
}

static void send_env(const unsigned char *payload, size_t plen,
                     unsigned cmd, unsigned cmd2, unsigned extra)
{
    unsigned char msg[64];
    size_t n = 0;
    mqd_t mq;

    if (!g_mq_ok)
        return;
    memset(msg, 0, sizeof(msg));
    put32(msg + 0, 1);
    put32(msg + 4, 8);
    put16(msg + 8, cmd);
    put16(msg + 10, cmd2);
    n = 12;
    if (plen) {
        memcpy(msg + n, payload, plen);
        n += plen;
    }
    if (extra) {
        put32(msg + n, extra);
        n += 4;
    }
    mq = mq_open(g_mq, O_WRONLY);
    if (mq == (mqd_t)-1) {
        if (g_mq_ok) {
            fprintf(stderr, "onvif: mq_open %s failed: %s - PTZ disabled\n",
                    g_mq, strerror(errno));
            g_mq_ok = 0;
        }
        return;
    }
    if (mq_send(mq, (char *)msg, n, 0) != 0) {
        fprintf(stderr, "onvif: mq_send failed: %s\n", strerror(errno));
    }
    mq_close(mq);
}

static void ptz_move(unsigned dir)
{
    unsigned char p[12];

    put32(p + 0, 24);
    put32(p + 4, dir);
    put32(p + 8, 0);
    send_env(p, sizeof(p), 0x4006, 0x4006, 0);
}

static void ptz_stop(void)
{
    unsigned char z[4] = {0, 0, 0, 0};

    send_env(z, sizeof(z), 0x4007, 0x0001, 0);
}

static void ptz_preset(unsigned cmd, unsigned index)
{
    unsigned char p[8];

    put32(p, 4);
    put32(p + 4, index);
    send_env(p, sizeof(p), cmd, 0x0001, 0);
}

/* ---- the 10 s unattended-move cap (single-threaded: checked in the
 * accept loop) ---- */
static time_t g_move_at;

/* ---- tiny XML helpers: targeted extraction ---- */
static const char *xml_find(const char *body, const char *tag)
{
    char open[64], close[64];
    const char *p, *end;

    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    p = strstr(body, open);
    if (!p)
        return NULL;
    p += strlen(open);
    end = strstr(p, close);
    if (!end)
        return NULL;
    /* copy into a static buffer: values are short and used immediately */
    static char val[256];
    size_t n = (size_t)(end - p);
    if (n > sizeof(val) - 1)
        n = sizeof(val) - 1;
    memcpy(val, p, n);
    val[n] = 0;
    return val;
}

/* extract an attribute value: x="..." inside a fragment like <a b="v"> */
static const char *xml_attr(const char *body, const char *attr)
{
    char pat[64];
    const char *p, *end;
    static char val[64];

    snprintf(pat, sizeof(pat), "%s=\"", attr);
    p = strstr(body, pat);
    if (!p)
        return NULL;
    p += strlen(pat);
    end = strchr(p, '"');
    if (!end || end - p > 63)
        return NULL;
    memcpy(val, p, (size_t)(end - p));
    val[end - p] = 0;
    return val;
}

/* ---- SOAP response plumbing ---- */
#define ENV_HEAD \
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" \
    "<SOAP-ENV:Envelope" \
    " xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"" \
    " xmlns:SOAP-ENC=\"http://www.w3.org/2003/05/soap-encoding\"" \
    " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"" \
    " xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\"" \
    " xmlns:tt=\"http://www.onvif.org/ver10/schema\"" \
    " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"" \
    " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"" \
    " xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\">" \
    "<SOAP-ENV:Body>"
#define ENV_TAIL "</SOAP-ENV:Body></SOAP-ENV:Envelope>"

static char g_resp[8192];

static void http_reply(int fd, const char *body)
{
    char hdr[256];
    int len = strlen(body);

    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: application/soap+xml; charset=utf-8\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n\r\n",
             len);
    write(fd, hdr, strlen(hdr));
    write(fd, body, len);
}

static void http_fault(int fd, const char *reason)
{
    char body[1024];

    snprintf(body, sizeof(body),
             ENV_HEAD
             "<SOAP-ENV:Fault><SOAP-ENV:Code>"
             "<SOAP-ENV:Value>SOAP-ENV:Receiver</SOAP-ENV:Value>"
             "</SOAP-ENV:Code><SOAP-ENV:Reason>"
             "<SOAP-ENV:Text xml:lang=\"en\">%s</SOAP-ENV:Text>"
             "</SOAP-ENV:Reason></SOAP-ENV:Fault>"
             ENV_TAIL,
             reason);
    http_reply(fd, body);
}

/* the XAddr base for capability replies */
static char g_xaddr[128];

static void mk_xaddr(void)
{
    snprintf(g_xaddr, sizeof(g_xaddr), "http://%s:%d",
             "10.1.2.19", g_port);   /* TODO: real address discovery */
}

/* the camera's real MAC - HA appends it to the device name */
static char g_mac[18] = "00:00:00:00:00:01";

static void mk_mac(void)
{
    FILE *f = fopen("/sys/class/net/wlan0/address", "r");
    char buf[32];

    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            size_t n = strcspn(buf, "\r\n");
            if (n == 17)
                memcpy(g_mac, buf, 17);
        }
        fclose(f);
    }
}

/* ---- the method handlers ---- */
static void rsp_device_information(int fd)
{
    snprintf(g_resp, sizeof(g_resp),
             ENV_HEAD
             "<tds:GetDeviceInformationResponse>"
             "<tds:Manufacturer>YI Technology</tds:Manufacturer>"
             "<tds:Model>YI Outdoor Camera 1080p</tds:Model>"
             "<tds:FirmwareVersion>5.0.00.00_202204281015</tds:FirmwareVersion>"
             "<tds:SerialNumber>b221fp</tds:SerialNumber>"
             "<tds:HardwareId>b221fp</tds:HardwareId>"
             "</tds:GetDeviceInformationResponse>"
             ENV_TAIL);
    http_reply(fd, g_resp);
}

static void rsp_capabilities(int fd)
{
    snprintf(g_resp, sizeof(g_resp),
             ENV_HEAD
             "<tds:GetCapabilitiesResponse><tds:Capabilities>"
             "<tt:Device><tt:XAddr>%s/onvif/device_service</tt:XAddr>"
             "<tt:Network><tt:IPFilter>false</tt:IPFilter>"
             "<tt:ZeroConfiguration>false</tt:ZeroConfiguration>"
             "</tt:Network></tt:Device>"
             "<tt:Media><tt:XAddr>%s/onvif/media_service</tt:XAddr>"
             "<tt:StreamingCapabilities><tt:RTSPStreaming>true</tt:RTSPStreaming>"
             "</tt:StreamingCapabilities></tt:Media>"
             "<tt:PTZ><tt:XAddr>%s/onvif/ptz_service</tt:XAddr></tt:PTZ>"
             "</tds:Capabilities></tds:GetCapabilitiesResponse>"
             ENV_TAIL,
             g_xaddr, g_xaddr, g_xaddr);
    http_reply(fd, g_resp);
}

static void rsp_scopes(int fd)
{
    snprintf(g_resp, sizeof(g_resp),
             ENV_HEAD
             "<tds:GetScopesResponse><tds:Scopes>"
             "<tt:ScopeDef>Fixed</tt:ScopeDef>"
             "<tt:ScopeItem>onvif://www.onvif.org/name/YI-Outdoor</tt:ScopeItem>"
             "</tds:Scopes></tds:GetScopesResponse>"
             ENV_TAIL);
    http_reply(fd, g_resp);
}

static void rsp_datetime(int fd)
{
    time_t t = time(NULL);
    struct tm tm;

    gmtime_r(&t, &tm);
    snprintf(g_resp, sizeof(g_resp),
             ENV_HEAD
             "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime>"
             "<tt:DateTimeType>Manual</tt:DateTimeType>"
             "<tt:DaylightSavings>false</tt:DaylightSavings>"
             "<tt:TimeZone><tt:TZ>GMT0</tt:TZ></tt:TimeZone>"
             "<tt:UTCDateTime><tt:Time><tt:Hour>%d</tt:Hour>"
             "<tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second>"
             "</tt:Time><tt:Date><tt:Year>%d</tt:Year>"
             "<tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date>"
             "</tt:UTCDateTime></tds:SystemDateAndTime>"
             "</tds:GetSystemDateAndTimeResponse>"
             ENV_TAIL,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    http_reply(fd, g_resp);
}

static void rsp_profiles(int fd)
{
    snprintf(g_resp, sizeof(g_resp),
             ENV_HEAD
             "<trt:GetProfilesResponse>"
             "<trt:Profiles token=\"main_stream\" fixed=\"true\">"
             "<tt:Name>main_stream</tt:Name>"
             "<tt:VideoSourceConfiguration token=\"video_src\">"
             "<tt:Name>video_src</tt:Name>"
             "<tt:UseCount>1</tt:UseCount>"
             "<tt:SourceToken>video_src</tt:SourceToken>"
             "<tt:Bounds x=\"0\" y=\"0\" width=\"1920\" height=\"1088\"/>"
             "</tt:VideoSourceConfiguration>"
             "<tt:AudioSourceConfiguration token=\"audio_src\">"
             "<tt:Name>audio_src</tt:Name>"
             "<tt:UseCount>1</tt:UseCount>"
             "<tt:SourceToken>audio_src</tt:SourceToken>"
             "</tt:AudioSourceConfiguration>"
             "<tt:VideoEncoderConfiguration token=\"video_enc\">"
             "<tt:Name>video_enc</tt:Name>"
             "<tt:UseCount>1</tt:UseCount>"
             "<tt:Encoding>H264</tt:Encoding>"
             "<tt:Resolution><tt:Width>1920</tt:Width>"
             "<tt:Height>1088</tt:Height></tt:Resolution>"
             "<tt:Quality>4</tt:Quality>"
             "<tt:RateControl><tt:FrameRateLimit>20</tt:FrameRateLimit>"
             "<tt:EncodingInterval>1</tt:EncodingInterval>"
             "<tt:BitrateLimit>2000</tt:BitrateLimit></tt:RateControl>"
             "<tt:H264><tt:GovLength>30</tt:GovLength>"
             "<tt:H264Profile>High</tt:H264Profile></tt:H264>"
             "<tt:SessionTimeout>PT10S</tt:SessionTimeout>"
             "</tt:VideoEncoderConfiguration>"
             "<tt:AudioEncoderConfiguration token=\"audio_enc\">"
             "<tt:Name>audio_enc</tt:Name>"
             "<tt:UseCount>1</tt:UseCount>"
             "<tt:Encoding>AAC</tt:Encoding>"
             "<tt:Bitrate>22</tt:Bitrate>"
             "<tt:SampleRate>16000</tt:SampleRate>"
             "</tt:AudioEncoderConfiguration>"
             "<tt:PTZConfiguration token=\"ptz\">"
             "<tt:Name>ptz</tt:Name>"
             "<tt:UseCount>1</tt:UseCount>"
             "<tt:NodeToken>ptz_node</tt:NodeToken>"
             "<tt:DefaultAbsolutePantTiltPositionSpace>"
             "http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace"
             "</tt:DefaultAbsolutePantTiltPositionSpace>"
             "<tt:DefaultRelativePanTiltTranslationSpace>"
             "http://www.onvif.org/ver10/tptz/PanTiltSpaces/TranslationGenericSpace"
             "</tt:DefaultRelativePanTiltTranslationSpace>"
             "<tt:DefaultContinuousPanTiltVelocitySpace>"
             "http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace"
             "</tt:DefaultContinuousPanTiltVelocitySpace>"
             "</tt:PTZConfiguration>"
             "</trt:Profiles>"
             "</trt:GetProfilesResponse>"
             ENV_TAIL);
    http_reply(fd, g_resp);
}

static void rsp_stream_uri(int fd)
{
    snprintf(g_resp, sizeof(g_resp),
             ENV_HEAD
             "<trt:GetStreamUriResponse><trt:MediaUri>"
             "<tt:Uri>rtsp://10.1.2.19:554/ch0_0.h264</tt:Uri>"
             "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
             "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
             "<tt:Timeout>PT10S</tt:Timeout>"
             "</trt:MediaUri></trt:GetStreamUriResponse>"
             ENV_TAIL);
    http_reply(fd, g_resp);
}

static void rsp_ptz_move(int fd, const char *body)
{
    const char *pt = strstr(body, "PanTilt");
    float x = 0, y = 0;

    if (pt) {
        const char *xv = xml_attr(pt, "x");
        const char *yv = xml_attr(pt, "y");
        if (xv)
            x = strtof(xv, NULL);
        if (yv)
            y = strtof(yv, NULL);
    }
    if (x == 0 && y == 0) {
        ptz_stop();
    } else if (x * x >= y * y) {
        ptz_move(x > 0 ? 4 : 3);   /* right : left */
    } else {
        ptz_move(y > 0 ? 1 : 2);   /* up : down */
    }
    g_move_at = time(NULL);
    snprintf(g_resp, sizeof(g_resp),
             ENV_HEAD "<tptz:ContinuousMoveResponse/>" ENV_TAIL);
    http_reply(fd, g_resp);
}

static void rsp_ptz_stop(int fd)
{
    ptz_stop();
    g_move_at = 0;
    snprintf(g_resp, sizeof(g_resp),
             ENV_HEAD "<tptz:StopResponse/>" ENV_TAIL);
    http_reply(fd, g_resp);
}

/* preset index table: token -> app preset index (1..4) */
static int preset_index(const char *token)
{
    const char *p = strrchr(token, '_');
    int i;

    if (p && sscanf(p + 1, "%d", &i) == 1 && i >= 1 && i <= 4)
        return i;
    return 1;
}

static void rsp_ptz_set_preset(int fd, const char *body)
{
    const char *tok = xml_find(body, "tt:PresetToken");

    ptz_preset(0x4000, 0);        /* the app stores the current position */
    (void)tok;
    snprintf(g_resp, sizeof(g_resp),
             ENV_HEAD
             "<tptz:SetPresetResponse><tptz:PresetToken>%s"
             "</tptz:PresetToken></tptz:SetPresetResponse>"
             ENV_TAIL,
             tok ? tok : "preset_1");
    http_reply(fd, g_resp);
}

static void rsp_ptz_goto_preset(int fd, const char *body)
{
    const char *tok = xml_find(body, "tt:PresetToken");

    ptz_preset(0x4002, (unsigned)preset_index(tok ? tok : "preset_1"));
    snprintf(g_resp, sizeof(g_resp),
             ENV_HEAD "<tptz:GotoPresetResponse/>" ENV_TAIL);
    http_reply(fd, g_resp);
}

static void rsp_ptz_get_presets(int fd)
{
    snprintf(g_resp, sizeof(g_resp),
             ENV_HEAD "<tptz:GetPresetsResponse/>" ENV_TAIL);
    http_reply(fd, g_resp);
}

static void rsp_ptz_status(int fd)
{
    snprintf(g_resp, sizeof(g_resp),
             ENV_HEAD
             "<tptz:GetStatusResponse><tptz:PTZStatus>"
             "<tt:Position><tt:PanTilt x=\"0\" y=\"0\"/></tt:Position>"
             "<tt:MoveStatus><tt:PanTilt>IDLE</tt:PanTilt></tt:MoveStatus>"
             "<tt:UtcTime>1970-01-01T00:00:00Z</tt:UtcTime>"
             "</tptz:PTZStatus></tptz:GetStatusResponse>"
             ENV_TAIL);
    http_reply(fd, g_resp);
}

/* the PTZ node, shared by GetNodes/GetNode: the spaces must match the
 * profile's PTZConfiguration defaults (VelocityGenericSpace) or
 * clients refuse the PTZ platform */
static const char *ptz_node_xml(void)
{
    static char node[2048];

    snprintf(node, sizeof(node),
             "<tptz:PTZNode token=\"ptz_node\" FixedHomePosition=\"false\">"
             "<tt:Name>ptz_node</tt:Name>"
             "<tt:SupportedPTZSpaces>"
             "<tt:ContinuousPanTiltVelocitySpace>"
             "<tt:URI>http://www.onvif.org/ver10/tptz/PanTiltSpaces/"
             "VelocityGenericSpace</tt:URI>"
             "<tt:XRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:XRange>"
             "<tt:YRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:YRange>"
             "</tt:ContinuousPanTiltVelocitySpace>"
             "</tt:SupportedPTZSpaces>"
             "<tt:MaximumNumberOfPresets>4</tt:MaximumNumberOfPresets>"
             "<tt:HomeSupported>false</tt:HomeSupported>"
             "</tptz:PTZNode>");
    return node;
}

static void rsp_ptz_nodes(int fd)
{
    snprintf(g_resp, sizeof(g_resp),
             ENV_HEAD
             "<tptz:GetNodesResponse>%s</tptz:GetNodesResponse>"
             ENV_TAIL,
             ptz_node_xml());
    http_reply(fd, g_resp);
}

static void rsp_ptz_node(int fd)
{
    snprintf(g_resp, sizeof(g_resp),
             ENV_HEAD
             "<tptz:GetNodeResponse>%s</tptz:GetNodeResponse>"
             ENV_TAIL,
             ptz_node_xml());
    http_reply(fd, g_resp);
}

static void rsp_ptz_configurations(int fd)
{
    snprintf(g_resp, sizeof(g_resp),
             ENV_HEAD
             "<tptz:GetConfigurationsResponse>"
             "<tptz:PTZConfiguration token=\"ptz\">"
             "<tt:Name>ptz</tt:Name>"
             "<tt:UseCount>1</tt:UseCount>"
             "<tt:NodeToken>ptz_node</tt:NodeToken>"
             "<tt:DefaultAbsolutePantTiltPositionSpace>"
             "http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace"
             "</tt:DefaultAbsolutePantTiltPositionSpace>"
             "<tt:DefaultRelativePanTiltTranslationSpace>"
             "http://www.onvif.org/ver10/tptz/PanTiltSpaces/TranslationGenericSpace"
             "</tt:DefaultRelativePanTiltTranslationSpace>"
             "<tt:DefaultContinuousPanTiltVelocitySpace>"
             "http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace"
             "</tt:DefaultContinuousPanTiltVelocitySpace>"
             "</tptz:PTZConfiguration>"
             "</tptz:GetConfigurationsResponse>"
             ENV_TAIL);
    http_reply(fd, g_resp);
}

static void rsp_ptz_svc_caps(int fd)
{
    snprintf(g_resp, sizeof(g_resp),
             ENV_HEAD
             "<tptz:GetServiceCapabilitiesResponse><tptz:Capabilities>"
             "<tt:GetCompatibleConfigurations>false</tt:GetCompatibleConfigurations>"
             "<tt:MoveStatus>false</tt:MoveStatus>"
             "<tt:StatusPosition>false</tt:StatusPosition>"
             "</tptz:Capabilities></tptz:GetServiceCapabilitiesResponse>"
             ENV_TAIL);
    http_reply(fd, g_resp);
}

/* media + device service capabilities: real clients (HA) probe both */
static void rsp_media_svc_caps(int fd)
{
    snprintf(g_resp, sizeof(g_resp),
             ENV_HEAD
             "<trt:GetServiceCapabilitiesResponse><trt:Capabilities>"
             "<tt:ProfileCapabilities><tt:MaximumNumberOfProfiles>1"
             "</tt:MaximumNumberOfProfiles></tt:ProfileCapabilities>"
             "<tt:StreamingCapabilities><tt:RTPMulticast>false"
             "</tt:RTPMulticast><tt:RTP_TCP>true</tt:RTP_TCP>"
             "<tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP></tt:StreamingCapabilities>"
             "</trt:Capabilities></trt:GetServiceCapabilitiesResponse>"
             ENV_TAIL);
    http_reply(fd, g_resp);
}

static void rsp_device_svc_caps(int fd)
{
    snprintf(g_resp, sizeof(g_resp),
             ENV_HEAD
             "<tds:GetServiceCapabilitiesResponse><tds:Capabilities>"
             "<tds:Network><tt:IPFilter>false</tt:IPFilter>"
             "<tt:ZeroConfiguration>false</tt:ZeroConfiguration>"
             "<tt:IPVersion6>false</tt:IPVersion6>"
             "<tt:DynDNS>false</tt:DynDNS></tds:Network>"
             "<tds:System><tt:DiscoveryResolve>false</tt:DiscoveryResolve>"
             "<tt:DiscoveryBye>false</tt:DiscoveryBye>"
             "<tt:RemoteDiscovery>false</tt:RemoteDiscovery>"
             "<tt:SystemBackup>false</tt:SystemBackup>"
             "<tt:SystemLogging>false</tt:SystemLogging>"
             "<tt:FirmwareUpgrade>false</tt:FirmwareUpgrade>"
             "</tds:System>"
             "</tds:Capabilities></tds:GetServiceCapabilitiesResponse>"
             ENV_TAIL);
    http_reply(fd, g_resp);
}

/* the service directory: clients (HA) use GetServices to locate the
 * PTZ service - without it they never reach PTZ at all */
static void rsp_services(int fd)
{
    snprintf(g_resp, sizeof(g_resp),
             ENV_HEAD
             "<tds:GetServicesResponse>"
             "<tds:Service>"
             "<tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace>"
             "<tds:XAddr>%s/onvif/device_service</tds:XAddr>"
             "<tds:Version><tt:Major>2</tt:Major><tt:Minor>6</tt:Minor></tds:Version>"
             "</tds:Service>"
             "<tds:Service>"
             "<tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace>"
             "<tds:XAddr>%s/onvif/media_service</tds:XAddr>"
             "<tds:Version><tt:Major>2</tt:Major><tt:Minor>6</tt:Minor></tds:Version>"
             "</tds:Service>"
             "<tds:Service>"
             "<tds:Namespace>http://www.onvif.org/ver20/ptz/wsdl</tds:Namespace>"
             "<tds:XAddr>%s/onvif/ptz_service</tds:XAddr>"
             "<tds:Version><tt:Major>2</tt:Major><tt:Minor>6</tt:Minor></tds:Version>"
             "</tds:Service>"
             "</tds:GetServicesResponse>"
             ENV_TAIL,
             g_xaddr, g_xaddr, g_xaddr);
    http_reply(fd, g_resp);
}

static void rsp_network_interfaces(int fd)
{
    snprintf(g_resp, sizeof(g_resp),
             ENV_HEAD
             "<tds:GetNetworkInterfacesResponse><tds:NetworkInterfaces "
             "token=\"eth0\">"
             "<tt:Enabled>true</tt:Enabled>"
             "<tt:Info><tt:Name>eth0</tt:Name>"
             "<tt:HwAddress>%s</tt:HwAddress>"
             "<tt:MTU>1500</tt:MTU></tt:Info>"
             "<tt:IPv4><tt:Enabled>true</tt:Enabled>"
             "<tt:Config><tt:Manual>"
             "<tt:Address>10.1.2.19</tt:Address>"
             "<tt:PrefixLength>24</tt:PrefixLength>"
             "</tt:Manual></tt:Config></tt:IPv4>"
             "</tds:NetworkInterfaces></tds:GetNetworkInterfacesResponse>"
             ENV_TAIL,
             g_mac);
    http_reply(fd, g_resp);
}

/* ---- request routing ---- */
static int handle_request(int fd)
{
    char req[16384];
    ssize_t n = read(fd, req, sizeof(req) - 1);
    const char *action, *body;

    if (n <= 0)
        return -1;
    req[n] = 0;
    body = strstr(req, "\r\n\r\n");
    if (!body)
        body = strstr(req, "\n\n");
    if (!body) {
        http_fault(fd, "no body");
        return 0;
    }
    body += (body[0] == '\r') ? 4 : 2;

    /* route on the SOAPAction header, fall back to the method tag */
    action = NULL;
    {
        const char *a = strstr(req, "SOAPAction:");
        if (a) {
            a += 11;
            while (*a == ' ' || *a == '"')
                a++;
            static char act[128];
            size_t i = 0;
            while (a[i] && a[i] != '"' && a[i] != '\r' && i < 127)
                act[i] = a[i], i++;
            act[i] = 0;
            action = act;
        }
    }

#define ACT_SUFFIX(svc, m) \
    (action && strstr(action, svc m) != NULL)

    if (ACT_SUFFIX("/device/wsdl/", "GetDeviceInformation"))
        rsp_device_information(fd);
    else if (ACT_SUFFIX("/device/wsdl/", "GetCapabilities"))
        rsp_capabilities(fd);
    else if (ACT_SUFFIX("/device/wsdl/", "GetScopes"))
        rsp_scopes(fd);
    else if (ACT_SUFFIX("/device/wsdl/", "GetSystemDateAndTime"))
        rsp_datetime(fd);
    else if (ACT_SUFFIX("/media/wsdl/", "GetProfiles"))
        rsp_profiles(fd);
    else if (ACT_SUFFIX("/media/wsdl/", "GetStreamUri"))
        rsp_stream_uri(fd);
    else if (ACT_SUFFIX("/ptz/wsdl/", "ContinuousMove"))
        rsp_ptz_move(fd, body);
    else if (ACT_SUFFIX("/ptz/wsdl/", "Stop"))
        rsp_ptz_stop(fd);
    else if (ACT_SUFFIX("/ptz/wsdl/", "SetPreset"))
        rsp_ptz_set_preset(fd, body);
    else if (ACT_SUFFIX("/ptz/wsdl/", "GotoPreset"))
        rsp_ptz_goto_preset(fd, body);
    else if (ACT_SUFFIX("/ptz/wsdl/", "GetPresets"))
        rsp_ptz_get_presets(fd);
    else if (ACT_SUFFIX("/ptz/wsdl/", "GetStatus"))
        rsp_ptz_status(fd);
    else if (ACT_SUFFIX("/ptz/wsdl/", "GetNodes"))
        rsp_ptz_nodes(fd);
    else if (ACT_SUFFIX("/ptz/wsdl/", "GetNode"))
        rsp_ptz_node(fd);
    else if (ACT_SUFFIX("/ptz/wsdl/", "GetConfigurations"))
        rsp_ptz_configurations(fd);
    else if (ACT_SUFFIX("/ptz/wsdl/", "GetServiceCapabilities"))
        rsp_ptz_svc_caps(fd);
    else if (ACT_SUFFIX("/media/wsdl/", "GetServiceCapabilities"))
        rsp_media_svc_caps(fd);
    else if (ACT_SUFFIX("/device/wsdl/", "GetServiceCapabilities"))
        rsp_device_svc_caps(fd);
    else if (ACT_SUFFIX("/device/wsdl/", "GetServices"))
        rsp_services(fd);
    else if (ACT_SUFFIX("/device/wsdl/", "GetNetworkInterfaces"))
        rsp_network_interfaces(fd);
    else {
        fprintf(stderr, "onvif: FAULT for %s\n", action ? action : "(no SOAPAction)");
        http_fault(fd, "method not implemented");
        return 0;
    }
    fprintf(stderr, "onvif: %s OK\n", action ? action : "(no SOAPAction)");
    return 0;
}

int main(int argc, char **argv)
{
    int lsock, csock, i;
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc)
            g_port = atoi(argv[++i]);
    }
    mk_xaddr();
    mk_mac();

    lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0) {
        perror("socket");
        return 1;
    }
    i = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(g_port);
    if (bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(lsock, 4) < 0) {
        perror("listen");
        return 1;
    }
    fprintf(stderr, "onvif: listening on port %d (device: %s/onvif/device_service)\n",
            g_port, g_xaddr);

    for (;;) {
        fd_set fds;
        struct timeval tv = {1, 0};

        FD_ZERO(&fds);
        FD_SET(lsock, &fds);
        if (select(lsock + 1, &fds, NULL, NULL, &tv) <= 0) {
            /* the 10 s unattended-move cap */
            if (g_move_at && time(NULL) - g_move_at > 10) {
                ptz_stop();
                g_move_at = 0;
                fprintf(stderr, "onvif: move cap fired - stopping\n");
            }
            continue;
        }
        csock = accept(lsock, (struct sockaddr *)&addr, &alen);
        if (csock < 0)
            continue;
        handle_request(csock);
        close(csock);
    }
}
