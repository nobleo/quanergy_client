#include "m8_client.h"
#include <iostream>
#include <ctime>
#include <boost/proto/args.hpp>

double const M8Client::M8_VERTICAL_ANGLES[] = { -0.318505, -0.2692, -0.218009, -0.165195, -0.111003, -0.0557982, 0, 0.0557982 };

M8Client::M8Client(const boost::asio::ip::address& ip,
                   const unsigned short int port)
  : data_queue_ ()
  , tcp_listener_endpoint_ (ip, port)
  , read_socket_service_ ()
  , read_socket_ (NULL)
  , queue_consumer_thread_ (NULL)
  , read_packet_thread_ (NULL)
  , current_sweep_xyzi_ (new pcl::PointCloud<pcl::PointXYZI>)
  , sweep_xyzi_signal_ ()
  , last_azimuth_ (65000)
  , sweep_counter_(0)
  , dropped_packets_ (0)
  , cos_lookup_table_(M8_NUM_ROT_ANGLES+1)
  , sin_lookup_table_(M8_NUM_ROT_ANGLES+1)
  , terminate_read_packet_thread_ (false)
{
  const double to_rad (M_PI / 180.f);
  for (unsigned int i = 0; i <= M8_NUM_ROT_ANGLES; i++)
  {
    double rad = to_rad * ((double (i) / M8_NUM_ROT_ANGLES) * 360.f);
    cos_lookup_table_[i] = std::cos (rad);
    sin_lookup_table_[i] = std::sin (rad);
  }

  const double* angle_in_radians = M8_VERTICAL_ANGLES;
  for (int i = 0; i < M8_NUM_LASERS; ++i, ++angle_in_radians)
  {
    sin_vertical_angles_[i] = std::sin (*angle_in_radians);
    cos_vertical_angles_[i] = std::cos (*angle_in_radians);
  }

  sweep_xyzi_signal_ = pcl::Grabber::createSignal<sig_cb_quanergy_m8_sweep_point_cloud_xyzi> ();
}

/////////////////////////////////////////////////////////////////////////////
M8Client::~M8Client () throw ()
{
  stop ();

  disconnect_all_slots<sig_cb_quanergy_m8_sweep_point_cloud_xyzi> ();
}

/////////////////////////////////////////////////////////////////////////////
bool
M8Client::isRunning () const
{
  return (!data_queue_.isEmpty() || read_packet_thread_ != NULL);
}

/////////////////////////////////////////////////////////////////////////////
std::string
M8Client::getName () const
{
  return (std::string ("Quanergy M8 LiDAR Grabber"));
}

/////////////////////////////////////////////////////////////////////////////
float
M8Client::getFramesPerSecond () const
{
  return (0.0f);
}

/////////////////////////////////////////////////////////////////////////////
void
M8Client::enqueueM8Packet(const unsigned char *data,
                          const std::size_t& bytes_received)
{
  if (bytes_received == M8_PACKET_BYTES)
  {
    if (data_queue_.size() > 1000)
    {
      ++dropped_packets_;
      if (!(dropped_packets_&(dropped_packets_-1)))
      {
        std::cerr << "enqueueM8Packet: dropped a total of "
                  << dropped_packets_ << " packets due to full buffer." << std::endl;
      }
      return;
    }
    unsigned char *dup = new unsigned char [bytes_received];
    memcpy (dup, data, bytes_received * sizeof(unsigned char));
    data_queue_.enqueue (dup);
  }
}

/////////////////////////////////////////////////////////////////////////////
void
M8Client::read()
{
  unsigned char data[8000];

  while (!terminate_read_packet_thread_ && read_socket_->is_open())
  {
    try
    {
      size_t length = boost::asio::read(*read_socket_, boost::asio::buffer (data, sizeof(M8DataPacket)));
      enqueueM8Packet (data, length);
    }
    catch (boost::system::system_error eof)
    {
      std::cerr << eof.code().message() << std::endl;
      terminate_read_packet_thread_ = true;
    }
  }
}


/////////////////////////////////////////////////////////////////////////////
void
M8Client::processM8Packets()
{
  while (true)
  {
    unsigned char *data;
    if (!data_queue_.dequeue (data))
      return;

    toPointClouds (reinterpret_cast<M8DataPacket*> (data));
    free (data);
  }
}

/////////////////////////////////////////////////////////////////////////////
void
M8Client::organizeCloud(PointCloudPtr &current_xyzi)
{
  // transpose the cloud
  PointCloudPtr temp_xyzi(new PointCloud);

  // reserve space
  temp_xyzi->reserve(current_xyzi->size());

  unsigned int temp_index;
  unsigned int width = current_xyzi->size() / M8_NUM_LASERS; // CONSTANT FOR NUM BEAMS

  // iterate through each ring from top down
  for (int i = M8_NUM_LASERS - 1; i >= 0; --i)
  {
    // iterate through width in collect order
    for (unsigned int j = 0; j < width; ++j)
    {
      // original data is in collect order and laser order
      temp_index = j * M8_NUM_LASERS;
      if (M8_NUM_LASERS == 8)
        temp_index += i;

      temp_xyzi->push_back(current_xyzi->at(temp_index));
    }
  }

  current_xyzi.swap(temp_xyzi);

  current_xyzi->height = M8_NUM_LASERS;
  current_xyzi->width  = width;
}

/////////////////////////////////////////////////////////////////////////////
void
M8Client::toPointClouds(M8DataPacket *data_packet)
{
  time_t time;
#if CLIENT_TIME
  namespace bpt = boost::posix_time;
  namespace bg = boost::gregorian;

  bpt::time_duration dur = bpt::microsec_clock::universal_time() - bpt::ptime(bg::date(1970, 1, 1));
  time = static_cast<uint32_t>(dur.total_seconds()) * 1e9 + static_cast<uint32_t>(dur.fractional_seconds() * 1e3);
#else
  time = data_packet->seconds * 1e9 + data_packet->nanoseconds;
#endif

#ifdef DEBUG
  std::cerr << "Packet at time: " << time;
#endif

  scan_counter_++;

  bool spin = (abs(data_packet->data[0].position - data_packet->data[M8_FIRING_PER_PKT-1].position) < (M8_FIRING_PER_PKT / 10)) ? 0 : 1;
  int direction = 0;

  if (spin)
  {
    if (data_packet->data[0].position - data_packet->data[M8_FIRING_PER_PKT-1].position > 0)
      direction = (data_packet->data[0].position - data_packet->data[M8_FIRING_PER_PKT-1].position > 4000) ? 1 : -1;
    else
      direction = (data_packet->data[M8_FIRING_PER_PKT-1].position - data_packet->data[0].position > 4000) ? 1 : -1;
  }
  else
    direction = 1;

  for (int i = 0; i < M8_FIRING_PER_PKT; ++i)
  {
    M8FiringData &data = data_packet->data[i];

    if (!spin)
      data.position = (scan_counter_*M8_FIRING_PER_PKT+i) % (1000);

    // calculate the angle in degrees
    double azimuth_angle = (static_cast<double> ((data.position+(M8_NUM_ROT_ANGLES/2))%M8_NUM_ROT_ANGLES) / (M8_NUM_ROT_ANGLES) * 360.0) - 180.;
    // check that the sensor is not spinning backward
    if (direction * azimuth_angle < direction * last_azimuth_)
    {
      if (current_sweep_xyzi_->size () > 0)
      {
        // transpose data
        // if (organize_data_)
        // {
        organizeCloud(current_sweep_xyzi_);
        // }

        current_sweep_xyzi_->header.stamp = time;
        current_sweep_xyzi_->header.seq = sweep_counter_;

        sweep_counter_++;
        // fire the signal that we have a new cloud
        fireCurrentSweep ();
#ifdef DEBUG
        std::cerr << "CLOUD @ PACKET #" << scan_counter_
                  << " AND ANGLES " << last_azimuth_ << " / " << azimuth_angle
                  << " AND SIZE " << current_sweep_xyzi_->size()
                  << " at " << current_sweep_xyzi_->header.stamp;
#endif
      }
      // start a new cloud
      current_sweep_xyzi_.reset (new PointCloud);
      // at first we assume it is dense
      current_sweep_xyzi_->is_dense = true;
    }

    // get the cosine corresponding
    const double cos_horizontal_angle = cos_lookup_table_[data.position];
    // get the sine corresponding
    const double sin_horizontal_angle = sin_lookup_table_[data.position];

    for (int j = 0; j < M8_NUM_LASERS; j++)
    {
      // convert range to meters
      float range = data.returns_distances[0][j] * .01;
      // output point
      pcl::PointXYZI xyzi;
      // convert to cartezian coordinates and populate x, y and z members
      computeXYZ (range, cos_horizontal_angle, sin_horizontal_angle,
		  cos_vertical_angles_[j], sin_vertical_angles_[j], xyzi);
      // intensity value is fetched directly
      xyzi.intensity = data.returns_intensities[0][j];
      // add the point to the current scan
      current_sweep_xyzi_->push_back (xyzi);
      // if the range is NaN, the cloud is not dense, one point is sufficient
      if (current_sweep_xyzi_->is_dense && std::isnan(range))
        current_sweep_xyzi_->is_dense = false;
    }

    last_azimuth_ = azimuth_angle;
  }
}

/////////////////////////////////////////////////////////////////////////////
void
M8Client::fireCurrentSweep()
{
  if (sweep_xyzi_signal_->num_slots () > 0)
    sweep_xyzi_signal_->operator() (current_sweep_xyzi_);
}

/////////////////////////////////////////////////////////////////////////////
void
M8Client::computeXYZ(const double range, 
		     const double cos_hz_angle, const double sin_hz_angle,
		     const double cos_vt_angle, const double sin_vt_angle, 
		     pcl::PointXYZI& point)
{
  if (std::isnan(range))
  {
    point.x = point.y = point.z = std::numeric_limits<float>::quiet_NaN();
    return;
  }

  // get the distance to the XY plane
  double xy_distance = range * cos_vt_angle - sin_vt_angle;
  // set y
  point.y = static_cast<float> (xy_distance * sin_hz_angle);
  // set x
  point.x = static_cast<float> (xy_distance * cos_hz_angle);
  // set z
  point.z = static_cast<float> (range * sin_vt_angle + cos_vt_angle);
}

/////////////////////////////////////////////////////////////////////////////

void M8Client::start()
{
  terminate_read_packet_thread_ = false;
  queue_consumer_thread_ = new boost::thread (boost::bind (&M8Client::processM8Packets, this));

  try
  {
    // We first try to connect to the given IP and port
    try
    {
      // create the read socket
      read_socket_ = new boost::asio::ip::tcp::socket (read_socket_service_);
      // try opening a connection with the sensor on the given IP
      read_socket_->open(boost::asio::ip::tcp::v4());
      // we don't need delays
      read_socket_->set_option(boost::asio::ip::tcp::no_delay(true));
      // establish the TCP connection
      read_socket_->connect(tcp_listener_endpoint_);
    }
    catch (boost::system::system_error bind) // if we fail, connect to any IP at that port
    {
      // first delete the previously created socket
      delete read_socket_;
      // create a new one
      read_socket_ = new boost::asio::ip::tcp::socket (read_socket_service_);
      read_socket_->open(boost::asio::ip::tcp::v4());
      read_socket_->set_option(boost::asio::ip::tcp::no_delay(true));
      tcp_listener_endpoint_ = boost::asio::ip::tcp::endpoint (boost::asio::ip::address_v4::any(), tcp_listener_endpoint_.port ());
      read_socket_->connect (tcp_listener_endpoint_);
    }
    // start the reading service
    read_socket_service_.run ();
  }
  catch (boost::system::system_error &e) // you can't recover from this
  {
    std::cerr << "Unable to bind to socket! " << e.code().message() << std::endl;
    return;
  }
  // At this point the connection has been established
  std::cout << "Sensor connected, starting to read packets" << std::endl;
  // Create the thread responsible for reading data from socket
  read_packet_thread_ = new boost::thread (boost::bind (&M8Client::read, this));
}

/////////////////////////////////////////////////////////////////////////////
void
M8Client::stop ()
{
  terminate_read_packet_thread_ = true;
  data_queue_.stopQueue ();

  if (read_packet_thread_ != NULL)
  {
    read_packet_thread_->interrupt ();
    read_packet_thread_->join ();
    delete read_packet_thread_;
    read_packet_thread_ = NULL;
  }

  if (queue_consumer_thread_ != NULL)
  {
    queue_consumer_thread_->join ();
    delete queue_consumer_thread_;
    queue_consumer_thread_ = NULL;
  }

  if (read_socket_ != NULL)
  {
    delete read_socket_;
    read_socket_ = NULL;
  }
}