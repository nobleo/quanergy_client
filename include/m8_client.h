#ifndef M8_CLIENT_H
#define M8_CLIENT_H

#include <boost/shared_ptr.hpp>
#include <boost/asio.hpp>

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/io/grabber.h>
#include <pcl/io/impl/synchronized_queue.hpp>

class M8Client : public pcl::Grabber, private boost::noncopyable
{
  public:
    /// The output is a PCL point cloud of type pcl::PointXYZI
    typedef pcl::PointCloud<pcl::PointXYZI> PointCloud;
    /// Const shared pointer
    typedef boost::shared_ptr<const PointCloud> PointCloudConstPtr;
    /// Shared pointer
    typedef boost::shared_ptr<PointCloud> PointCloudPtr;

    /** \brief Signal calllback used for a 360 degree sweep
      * Represents multiple corrected packets from the Quanergy M8.
      */
    typedef void (sig_cb_quanergy_m8_sweep_point_cloud_xyzi) (const PointCloudConstPtr&);

    /** \brief Constructor taking a specified IP/port.
      * \param[in] ip IP Address that should be used to listen for M8 packets
      * \param[in] port TCP Port that should be used to listen for M8 packets
      */
    M8Client (const boost::asio::ip::address& ip, const unsigned short port);

    /** \brief virtual Destructor inherited from the pcl::Grabber interface. It never throws. */
    virtual ~M8Client () throw ();

    /** \brief Starts processing the Quanergy M8 packets, either from the network or PCAP file. */
    virtual void start ();

    /** \brief Stops processing the Quanergy M8 packets, either from the network or PCAP file */
    virtual void stop ();

    /** \brief Obtains the name of this I/O grabber
     *  \return The name of the grabber
     */
    virtual std::string getName () const;

    /** \brief Check if the grabber is still running.
     *  \return TRUE if the grabber is running, FALSE otherwise
     */
    virtual bool isRunning () const;

    /** \brief Returns the number of frames per second.
     */
    virtual float getFramesPerSecond () const;

  private:
    /// Default TCP port for the M8 sensor
    static const int M8_DATA_PORT = 4141;
    /// Default angles
    static const int M8_NUM_ROT_ANGLES = 10400;
    /// Default number of firings per TCP packet
    static const int M8_FIRING_PER_PKT = 50;
    /// Size of TCP packet
    static const int M8_PACKET_BYTES = 6612;
    /// Ultimately M8 would be a multiecho LiDAR, for now only the first echo is available
    static const int M8_NUM_RETURNS = 3;
    /// The total number of lasers on the M8 Sensor
    static const int M8_NUM_LASERS = 8;
    /// Vertical angles
    static const double M8_VERTICAL_ANGLES[];
    /// Default IP address for the sensor
    static const boost::asio::ip::address M8_DEFAULT_NETWORK_ADDRESS;

    /// \brief structure that holds the sensor firing output
    struct M8FiringData
    {
      unsigned short position;
      unsigned short padding;
      unsigned int   returns_distances[M8_NUM_RETURNS][M8_NUM_LASERS];   // 32-bit, 1 cm resolution.
      unsigned char  returns_intensities[M8_NUM_RETURNS][M8_NUM_LASERS]; // 8-bit, 0-255
      unsigned char  returns_status[M8_NUM_LASERS];                      // 8-bit, 0-255
    }; // 132 bytes

    /// \brief structure that holds multiple sensor firings and gets sent in the TCP packet
    struct M8DataPacket
    {
      M8FiringData data[M8_FIRING_PER_PKT];
      unsigned int seconds;     // 32-bit, seconds from Jan 1 1970
      unsigned int nanoseconds; // 32-bit, fractional seconds turned to nanoseconds
      unsigned int status;      // 32-bit, undefined for now
    }; // 6612 bytes

    /// function used as a callback for the thread that enqueus encoming data in the queue
    void enqueueM8Packet(const unsigned char *data,
                         const std::size_t& bytes_received);
    /// function used as a callback for the socket reading thread
    void read();
    /// processes the TCP packets
    void processM8Packets();
    /// transposes the point cloud
    void organizeCloud(PointCloudPtr &current_xyzi);
    /// converts TCP packets to PCL point clouds
    void toPointClouds(M8DataPacket *data_packet);
    /// Fire current sweep
    void fireCurrentSweep();
    /** Convert from range and angles to cartesian
      * \param[in] range range in meter
      * \param[in] cos_hz_angle cosine of horizontal angle
      * \param[in] sin_hz_angle sinine of horizontal angle
      * \param[in] cos_vt_angle cosine of vertical angle
      * \param[in] sin_vt_angle sinine of vertical angle
      * \param[out] point point in cartesian coordinates
      */
    void computeXYZ(const double range, 
		    const double cos_hz_angle, const double sin_hz_angle,
		    const double cos_vt_angle, const double sin_vt_angle, 
		    pcl::PointXYZI& point);
    /// sensor IP address
    boost::asio::ip::address ip_address_;
    /// TCP port
    unsigned int port_;
    /// TCP socket to the sensor, using for reading
    boost::asio::ip::tcp::socket *read_socket_;
    /// TCP socket reading service
    boost::asio::io_service read_socket_service_;
    /// TCP end point
    boost::asio::ip::tcp::endpoint tcp_listener_endpoint_;
    /// SynchronizedQueue is a thread-safe access queue
    pcl::SynchronizedQueue<unsigned char *> data_queue_;
    /// lookup table for cosinus
    std::vector<double> cos_lookup_table_;
    /// lookup table for sinus
    std::vector<double> sin_lookup_table_;
    double cos_vertical_angles_[M8_NUM_LASERS];
    double sin_vertical_angles_[M8_NUM_LASERS];
    /// queue consuming thread
    boost::thread *queue_consumer_thread_;
    /// packet reading thread
    boost::thread *read_packet_thread_;
    /// termination condition
    bool terminate_read_packet_thread_;
    boost::shared_ptr<pcl::PointCloud<pcl::PointXYZI> > current_sweep_xyzi_;
    /// signal that gets fired whenever we collect a scan
    boost::signals2::signal<sig_cb_quanergy_m8_sweep_point_cloud_xyzi>* sweep_xyzi_signal_;
    /// last accounted for azimuth angle
    double last_azimuth_;
    /// scan height
    int scan_height_;
    /// sweep height
    int sweep_height_;
    /// encoder
    int encoder0;
    /// encoder
    int encoder1;
    /// spinning direction
    short direction_;
    /// range offset file ???
    std::string rangeOffsetFile_;
    int rangeOffset_;
    /// number of dropped packets
    int dropped_packets_;
    /// global scan counter
    uint32_t scan_counter_;
    /// global sweep counter
    uint32_t sweep_counter_;
};

#endif // M8_CLIENT_H
