/*
  Copyright <2018-2020> <Ainstein, Inc.>

  Redistribution and use in source and binary forms, with or without modification, are permitted 
  provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this list of 
  conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice, this list of 
  conditions and the following disclaimer in the documentation and/or other materials provided 
  with the distribution.

  3. Neither the name of the copyright holder nor the names of its contributors may be used to 
  endorse or promote products derived from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR 
  IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND 
  FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR 
  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL 
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
  IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY 
  OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <iostream>
#include <cstdint>
#include <cstring>
#include <cerrno>

#include "ainstein_radar_drivers/radar_driver_o79_udp.h"

namespace ainstein_radar_drivers
{

  const std::string RadarDriverO79UDP::connect_cmd_str = std::string( "connect" );
  const unsigned int RadarDriverO79UDP::connect_res_len = 18;
  
  const std::string RadarDriverO79UDP::run_cmd_str = std::string( "run" );

  const unsigned int RadarDriverO79UDP::max_msg_len = 3000; // maximum length in bytes
  const unsigned int RadarDriverO79UDP::msg_len_raw_targets = 8; // 8 bytes per raw target
  const unsigned int RadarDriverO79UDP::msg_len_tracked_targets = 8; // 8 bytes per raw target
  const unsigned int RadarDriverO79UDP::msg_len_bounding_boxes = 9; // 9 bytes per bounding box
  const unsigned int RadarDriverO79UDP::msg_len_tracked_targets_cart = 12; // 12 bytes per Cartesian tracked target
  
  const unsigned int RadarDriverO79UDP::msg_header_len = 8; // 8 byte header per message

  const unsigned int RadarDriverO79UDP::msg_id_raw_targets = 0x00;
  const unsigned int RadarDriverO79UDP::msg_id_tracked_targets = 0x01;
  const unsigned int RadarDriverO79UDP::msg_id_bounding_boxes = 0x02;
  const unsigned int RadarDriverO79UDP::msg_id_tracked_targets_cart = 0x04;
  
  RadarDriverO79UDP::RadarDriverO79UDP( std::string host_ip_address, int host_port,
		  std::string radar_ip_address, int radar_port ) :
    host_ip_addr_( host_ip_address ),
    host_port_( host_port ),
    radar_ip_addr_( radar_ip_address ),
    radar_port_( radar_port )
  {
    buffer_ = static_cast<char*>( malloc( RadarDriverO79UDP::max_msg_len * sizeof( char ) ) );
  }

  RadarDriverO79UDP::~RadarDriverO79UDP(void)
  {
    close( sockfd_ );
    free( buffer_ );
  }

  bool RadarDriverO79UDP::connect(void)
  {
    // Create the host UDP socket:
    sockfd_ = socket( AF_INET, SOCK_DGRAM, 0 );
    if( sockfd_ < 0 )
      {
	std::cout << "Failed to create socket." << std::endl;
	return false;
      }

    // Configure the host sockaddr using the host IP and port:
    memset( &sockaddr_, 0, sizeof( sockaddr_ ) );
    sockaddr_.sin_family = AF_INET;
    sockaddr_.sin_port = htons( host_port_ );
    sockaddr_.sin_addr.s_addr = inet_addr( host_ip_addr_.c_str() );

    // Configure the radar sockaddr using the host IP and port:
    memset( &destaddr_, 0, sizeof( destaddr_ ) );
    destaddr_.sin_port = htons( radar_port_ );
    destaddr_.sin_addr.s_addr = inet_addr( radar_ip_addr_.c_str() );

    // Set socket options:
    int reuseaddr = 1;
    int res = setsockopt( sockfd_, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof( reuseaddr ) );
    if( res < 0 )
      {
	std::cout << "Failed to set socket options: " << std::strerror( errno ) << std::endl;
	return false;
      }

    // Set socket timeout:
    struct timeval tv;
    tv.tv_sec = 3; // 3 second timeout
    tv.tv_usec = 0;
    res  = setsockopt( sockfd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) );
    if( res < 0 )
      {
	std::cout << "Failed to set socket timeout: " << std::strerror( errno ) << std::endl;
	return false;
      }
    
    // Explicitly bind the host UDP socket:
    res = bind( sockfd_, ( struct sockaddr * )( &sockaddr_ ), sizeof( sockaddr_ ) );
    if( res < 0 )
      {
	std::cout << "Failed to bind socket: " << std::strerror( errno ) << std::endl;
	return false;
      }

    // Try to receive data until the timeout to see if the O79 is already running:
    struct sockaddr_storage src_addr;
    socklen_t src_addr_len = sizeof( src_addr );
    res = recvfrom( sockfd_, ( char* )buffer_, RadarDriverO79UDP::max_msg_len, MSG_WAITALL, ( struct sockaddr *)( &src_addr ), &src_addr_len );
    if( res < 0 )
      {
	// If blocking recvfrom times out, errno is set to EAGAIN:
	if( errno == EAGAIN )
	  {
	    // Send the connect command to the radar:
	    RadarDriverO79UDP::connect_cmd_str.copy( buffer_, RadarDriverO79UDP::connect_cmd_str.length() );
	    res = sendto( sockfd_, ( char* )buffer_, RadarDriverO79UDP::connect_cmd_str.length(), 0, ( struct sockaddr *)( &destaddr_ ), sizeof( destaddr_ ) );
	    if( res < 0 )
	      {
		std::cout << "Failed to send connect command to radar: " << std::strerror( errno ) << std::endl;
		return false;
	      }

	    // Wait for a response to the connect command:
	    res = recvfrom( sockfd_, ( char* )buffer_, RadarDriverO79UDP::connect_res_len, MSG_WAITALL, ( struct sockaddr *)( &src_addr ), &src_addr_len );
	    if( res < 0 )
	      {
		std::cout << "Failed to receive connect response from radar: " << std::strerror( errno ) << std::endl;
		return false;
	      }
	    else
	      {
		// TODO: print settings stored on radar from connect response message
	      }

	    // Send the run command to the radar:
	    RadarDriverO79UDP::run_cmd_str.copy( buffer_, RadarDriverO79UDP::run_cmd_str.length() );
	    res = sendto( sockfd_, ( char* )buffer_, RadarDriverO79UDP::run_cmd_str.length(), 0, ( struct sockaddr *)( &destaddr_ ), sizeof( destaddr_ ) );
	    if( res < 0 )
	      {
		std::cout << "Failed to send run command to radar: " << std::strerror( errno ) << std::endl;
		return false;
	      }
	  }
	else // encountered some error other than timeout
	  {
	    std::cout << "Failed when attempting to detect whether radar is running: " << std::strerror( errno ) << std::endl;
	    return false;
	  }
      }
  
    return true;
  }

  bool RadarDriverO79UDP::receiveTargets( std::vector<ainstein_radar_drivers::RadarTarget> &targets,
					  std::vector<ainstein_radar_drivers::RadarTarget> &targets_tracked,
					  std::vector<ainstein_radar_drivers::BoundingBox> &bounding_boxes,
					  std::vector<ainstein_radar_drivers::RadarTargetCartesian> &targets_tracked_cart )
  {
    // Clear the targets array in preparation for message processing:
    targets.clear();
    targets_tracked.clear();
    bounding_boxes.clear();
    targets_tracked_cart.clear();
    
    // Received message length:
    int msg_len;

    // Received message data length (excluding header):
    int msg_data_len;
    
    // Structures to store where the UDP message came from:
    struct sockaddr_storage src_addr;
    socklen_t src_addr_len = sizeof( src_addr );

    // Call to block until data has been received:
    msg_len = recvfrom( sockfd_, ( char* )buffer_, RadarDriverO79UDP::max_msg_len, MSG_WAITALL, ( struct sockaddr *)( &src_addr ), &src_addr_len );

    if( msg_len < 0 )
      {
	std::cout << "Failed to read data: " << std::strerror( errno ) << std::endl;
	return false;
      }
    else
      {
	// Extract the sender's IP address:
	struct sockaddr_in* sin = ( struct sockaddr_in* )&src_addr;
	unsigned char* src_ip = ( unsigned char* )( &sin->sin_addr.s_addr );
	// printf("source IP: %d.%d.%d.%d\n", src_ip[0], src_ip[1], src_ip[2], src_ip[3]);

	// Compute length of actual data for checking payload size:
	msg_data_len = msg_len - RadarDriverO79UDP::msg_header_len;
	
	// Extract the target ID and data from the message:
	int offset;
	ainstein_radar_drivers::RadarTarget target;
	ainstein_radar_drivers::BoundingBox box;
	ainstein_radar_drivers::RadarTargetCartesian target_cart;

	// Check the first byte for the message type ID:
	if( buffer_[0] == RadarDriverO79UDP::msg_id_tracked_targets )
	  {
	    // Check data is a valid length:
	    if( ( msg_data_len % RadarDriverO79UDP::msg_len_tracked_targets ) != 0 )
	      {
		std::cout << "WARNING >> Incorrect number of bytes: " << msg_len << std::endl;
		return false;
	      }
	    else
	      {
		for( int i = 0; i < ( msg_data_len / RadarDriverO79UDP::msg_len_tracked_targets ); ++i )
		  {
		    // Offset per target includes header
		    offset = i * RadarDriverO79UDP::msg_len_tracked_targets + RadarDriverO79UDP::msg_header_len;

		    target.id = i;
		    target.azimuth =  static_cast<double>( static_cast<int16_t>( ( buffer_[offset + 1] & 0xff ) << 8 ) | static_cast<int16_t>( buffer_[offset + 0] & 0xff ) );
		    target.range = static_cast<uint8_t>( buffer_[offset + 2] ) * 0.116;   // 1 count = 0.1 m

		    // Speed is 0-127, with 0-64 negative (moving away) and 65-127 positive (moving towards).
		    // Note that 65 is the highest speed moving towards, hence the manipulation below.
		    if( static_cast<uint8_t>( buffer_[offset + 3] ) <= 64 ) // MOVING AWAY FROM RADAR
		      {
			target.speed = static_cast<uint8_t>( buffer_[offset + 3] ) * 0.045; // 1 count = 0.045 m/s
		      }
		    else // MOVING TOWARDS RADAR
		      {
			target.speed = ( static_cast<uint8_t>( buffer_[offset + 3] ) - 127 ) * 0.045; // 1 count = 0.045 m/s
		      }

		    target.elevation = static_cast<double>( static_cast<int16_t>( ( buffer_[offset + 5] & 0xff ) << 8 ) | static_cast<int16_t>( buffer_[offset + 4] & 0xff ) );
		    target.snr =  static_cast<double>( static_cast<uint16_t>( ( buffer_[offset + 7] & 0xff ) << 8 ) | static_cast<uint16_t>( buffer_[offset + 6] & 0xff ) );

		    targets_tracked.push_back( target );
		  }
	      }
	  }
	else if( buffer_[0] == RadarDriverO79UDP::msg_id_raw_targets )
	  {
	    for( int i = 0; i < ( msg_data_len / RadarDriverO79UDP::msg_len_raw_targets ); ++i )
	      {
		offset = i * RadarDriverO79UDP::msg_len_raw_targets + RadarDriverO79UDP::msg_header_len;

		target.id = i;
		target.azimuth =  static_cast<double>( static_cast<int16_t>( ( buffer_[offset + 1] & 0xff ) << 8 ) | static_cast<int16_t>( buffer_[offset + 0] & 0xff ) );
		target.range = static_cast<uint8_t>( buffer_[offset + 2] ) * 0.116;   // 1 count = 0.1 m

		// Speed is 0-127, with 0-64 negative (moving away) and 65-127 positive (moving towards).
		// Note that 65 is the highest speed moving towards, hence the manipulation below.
		if( static_cast<uint8_t>( buffer_[offset + 3] ) <= 64 ) // MOVING AWAY FROM RADAR
		  {
		    target.speed = static_cast<uint8_t>( buffer_[offset + 3] ) * 0.045; // 1 count = 0.045 m/s
		  }
		else // MOVING TOWARDS RADAR
		  {
		    target.speed = ( static_cast<uint8_t>( buffer_[offset + 3] ) - 127 ) * 0.045; // 1 count = 0.045 m/s
		  }

		target.elevation = static_cast<double>( static_cast<int16_t>( ( buffer_[offset + 5] & 0xff ) << 8 ) | static_cast<int16_t>( buffer_[offset + 4] & 0xff ) );
		target.snr =  static_cast<double>( static_cast<uint16_t>( ( buffer_[offset + 7] & 0xff ) << 8 ) | static_cast<uint16_t>( buffer_[offset + 6] & 0xff ) );

		targets.push_back( target );
	      }
	  }
	else if( buffer_[0] == RadarDriverO79UDP::msg_id_bounding_boxes )
	  {
	    for( int i = 0; i < ( msg_data_len / RadarDriverO79UDP::msg_len_bounding_boxes ); ++i )
	      {
		offset = i * RadarDriverO79UDP::msg_len_bounding_boxes + RadarDriverO79UDP::msg_header_len;

		// Compute box pose (identity orientation, geometric center is position):
		box.pose.linear() = Eigen::Matrix3d::Identity();
		box.pose.translation().x() = static_cast<double>( static_cast<int16_t>( ( buffer_[offset + 1] & 0xff ) << 8 ) | static_cast<int16_t>( buffer_[offset + 0] & 0xff ) ) * 0.1; 
		box.pose.translation().y() = static_cast<double>( static_cast<int16_t>( ( buffer_[offset + 3] & 0xff ) << 8 ) | static_cast<int16_t>( buffer_[offset + 2] & 0xff ) ) * 0.1; 
		box.pose.translation().z() = static_cast<double>( static_cast<int16_t>( ( buffer_[offset + 5] & 0xff ) << 8 ) | static_cast<int16_t>( buffer_[offset + 4] & 0xff ) ) * 0.1; 

		box.dimensions.x() = std::max( 0.1, static_cast<uint8_t>( buffer_[offset + 6] ) * 0.1 );
		box.dimensions.y() = std::max( 0.1, static_cast<uint8_t>( buffer_[offset + 7] ) * 0.1 );
		box.dimensions.z() = std::max( 0.1, static_cast<uint8_t>( buffer_[offset + 8] ) * 0.1);

		bounding_boxes.push_back( box );
	      }
	  }
	else if( buffer_[0] == RadarDriverO79UDP::msg_id_tracked_targets_cart )
	  {
	    // Check data is a valid length:
	    if( ( msg_data_len % RadarDriverO79UDP::msg_len_tracked_targets_cart ) != 0 )
	      {
		std::cout << "WARNING >> Incorrect number of bytes: " << msg_len << std::endl;
		return false;
	      }
	    else
	      {
		for( int i = 0; i < ( msg_data_len / RadarDriverO79UDP::msg_len_tracked_targets_cart ); ++i )
		  {
		    // Offset per target includes header
		    offset = i * RadarDriverO79UDP::msg_len_tracked_targets_cart + RadarDriverO79UDP::msg_header_len;

		    target_cart.pos.x() = static_cast<double>( static_cast<int16_t>( ( buffer_[offset + 1] & 0xff ) << 8 ) | static_cast<int16_t>( buffer_[offset + 0] & 0xff ) ) * 0.1; 
		    target_cart.pos.y() = static_cast<double>( static_cast<int16_t>( ( buffer_[offset + 3] & 0xff ) << 8 ) | static_cast<int16_t>( buffer_[offset + 2] & 0xff ) ) * 0.1; 
		    target_cart.pos.z() = static_cast<double>( static_cast<int16_t>( ( buffer_[offset + 5] & 0xff ) << 8 ) | static_cast<int16_t>( buffer_[offset + 4] & 0xff ) ) * 0.1; 

		    target_cart.vel.x() = static_cast<double>( static_cast<int16_t>( ( buffer_[offset + 7] & 0xff ) << 8 ) | static_cast<int16_t>( buffer_[offset + 6] & 0xff ) ) * 0.1; 
		    target_cart.vel.y() = static_cast<double>( static_cast<int16_t>( ( buffer_[offset + 9] & 0xff ) << 8 ) | static_cast<int16_t>( buffer_[offset + 8] & 0xff ) ) * 0.1; 
		    target_cart.vel.z() = static_cast<double>( static_cast<int16_t>( ( buffer_[offset + 11] & 0xff ) << 8 ) | static_cast<int16_t>( buffer_[offset + 10] & 0xff ) ) * 0.1; 

		    targets_tracked_cart.push_back( target_cart );
		  }
	      }
	    
	  }
	else
	  {
	    std::cout << "WARNING >> Message received with invalid ID." << std::endl;
	  }
      }

    return true;
  }

} // namespace ainstein_radar_drivers
