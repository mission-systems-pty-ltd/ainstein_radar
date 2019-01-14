#include "RadarDataVizPointCloud.h"

RadarDataVizPointCloud::RadarDataVizPointCloud( std::string data_topic, std::string vel_topic, std::string pcl_topic )
  : data_topic_( data_topic ),
    vel_topic_( vel_topic ),
    pcl_topic_( pcl_topic ),
    listen_tf_( buffer_tf_ )
    
{
  pub_pcl_ = node_handle_.advertise<sensor_msgs::PointCloud2>( pcl_topic_, 10 );
  sub_radar_data_ = node_handle_.subscribe( data_topic_, 10,
					    &RadarDataVizPointCloud::radarDataCallback,
					    this );
  sub_radar_vel_ = node_handle_.subscribe( vel_topic_, 10,
					   &RadarDataVizPointCloud::radarVelCallback,
					   this );

  // Get parameters:
  node_handle_.param( "max_speed_thresh", max_speed_thresh_, 1.0 );
  node_handle_.param( "min_dist_thresh", min_dist_thresh_, 1.0 );
  node_handle_.param( "max_dist_thresh", max_dist_thresh_, 20.0 );
  node_handle_.param( "compute_3d", compute_3d_, false );
  node_handle_.param( "is_rotated", is_rotated_, false );
  
  // Assume radar velocity is not available until a message is received:
  is_vel_available_ = false;
}

void RadarDataVizPointCloud::radarVelCallback( const geometry_msgs::Twist &msg )
{
  // Get the radar linear velocity in world frame:
  tf2::fromMsg( msg.linear, vel_world_ );
  is_vel_available_ = true;
}

void RadarDataVizPointCloud::radarDataCallback( const radar_sensor_msgs::RadarData &msg )
{
  // Get the data frame ID and look up the corresponding tf transform:
  Eigen::Affine3d tf_sensor_to_world =
    tf2::transformToEigen(buffer_tf_.lookupTransform( "base_link", msg.header.frame_id, ros::Time( 0 ) ) );
  
  // Clear the point cloud point vector:
  pcl_.clear();

  // Iterate through raw targets and add them to the point cloud:
  for( auto it = msg.raw_targets.begin(); it != msg.raw_targets.end(); ++it )
    {
      // If the radar world frame velocity is available from another source, use it for further processing:
      if( is_vel_available_ )
	{
	  // Copy the original radar target for further processing:
	  radar_sensor_msgs::RadarTarget t = *it;

	  // Copy azimuth to elevation if the radar has been rotated:
	  // In radar frame, +ve azimuth is LEFT, +ve elevation is DOWN.
	  // When rotated -90* about x (clockwise from front), swap azimuth and elevation:
	  if( is_rotated_ )
	    {
	      t.elevation = t.azimuth; // +ve azimith maps to +ve elevation when rotated
	      t.azimuth = 0.0;
	    }
	  
	  // Compute the velocity of the radar rotated into instantaneous radar frame:
	  Eigen::Vector3d vel_radar = -tf_sensor_to_world.linear().inverse() * vel_world_;

	  if( compute_3d_ )
	    {
	      // Use the radar velocity information and target relative speed to compute
	      // either election or azimuth (if rotated):
	      //
	      // s = n^{T} * R_{W}^{R} * ( v_{T}^{W} - v_{R}^{W} )
	      // s = -n^{T} * R_{W}^{R} * v_{R}^{W} assuming v_{T}^{W} = 0 (static target)
	      // -s = [cos(a) * cos(e), sin(a) * cos(e), sin(e)] * v_{R}^{R}
	      // -s = v_{T,x}^{R} * cos(a) * cos(e) +
	      //      v_{T,y}^{R} * sin(a) * cos(e) + 
	      //   	  v_{T,z}^{R} * sin(e)
	      //
	      // In either case, we solve an equation of the form x*cos(th)+y*sin(th)=z
	      // which has infinite solutions of the form:
	      //
	      // th = 2*arctan((1/(x+z)) * (y+/-sqrt(y^2+x^2-z^2))) + 2*pi*n for all n
	      //
	      // We take n=0 and choose the minimum (abs) of the two solutions
	      //
	      // th_m = 2*arctan((1/(x+z)) * (y-sqrt(y^2+x^2-z^2)))
	      // th_p = 2*arctan((1/(x+z)) * (y+sqrt(y^2+x^2-z^2)))

	      double x, y, z;
	      if( is_rotated_ )
		{
		  // Assuming elevation is known, solve for azimuth:
		  //
		  // v_{T,x}^{R} * cos(e) * cos(a) + v_{T,y}^{R} * cos(e) * sin(a) = -s - v_{T,z}^{R} * sin(e)
		  //
		  // Put in form x * cos(e) + y * sin(e) = z:
		  // x = v_{T,x}^{R} * cos(e)
		  // y = v_{T,y}^{R} * cos(e)
		  // z = -s - v_{T,z}^{R} * sin(e)
	      
		  x = vel_radar( 0 ) * cos( ( M_PI / 180.0 ) * t.elevation );
		  y = vel_radar( 1 ) * cos( ( M_PI / 180.0 ) * t.elevation );
		  z = t.speed - vel_radar( 2 ) * sin( ( M_PI / 180.0 ) * t.elevation ); // should be -t.speed with correct speed sign, fix

		  t.azimuth = solveForAngle( x, y, z );
		}
	      else
		{
		  // Assuming azimuth is known, solve for elevation:
		  //
		  // ( v_{T,x}^{R} * cos(a) + v_{T,y}^{R} * sin(a) ) * cos(e) + v_{T,z}^{R} * sin(e) = -s
		  //
		  // Put in form x * cos(e) + y * sin(e) = z:
		  // x = v_{T,x}^{R} * cos(a) + v_{T,y}^{R} * sin(a)
		  // y = v_{T,z}^{R}
		  // z = -s
	      
		  x = vel_radar( 0 ) * cos( ( M_PI / 180.0 ) * t.azimuth ) +
		    vel_radar( 1 ) * sin( ( M_PI / 180.0 ) * t.azimuth );
		  y = vel_radar( 2 ); // likely close to zero
		  z = t.speed; // should be -t.speed with correct speed sign, fix

		  t.elevation = solveForAngle( x, y, z );
		}
	    }
	  
	  // Compute the unit vector along the axis between sensor and target:
	  // n = [cos(azi)*cos(elev), sin(azi)*cos(elev), sin(elev)]
	  Eigen::Vector3d meas_dir = Eigen::Vector3d( cos( ( M_PI / 180.0 ) * t.azimuth ) * cos( ( M_PI / 180.0 ) * t.elevation ),
						      sin( ( M_PI / 180.0 ) * t.azimuth ) * cos( ( M_PI / 180.0 ) * t.elevation ),
						      sin( ( M_PI / 180.0 ) * t.elevation ) );

	  // Radar measures relative speed s of target along meas_dir.:
	  // s = n^{T} * R^{T} * ( v_{T} - v_{car} )
	  // We wish to filter out targets with nonzero world frame
	  // velocity v_{T}, but can only compute the projection of v_{T}:
	  // v_{T,proj} = n^{T} * R^{T} * v_{T} = s + n^{T} * R^{T} * v_{car}
	  double proj_speed = t.speed - meas_dir.dot( tf_sensor_to_world.linear().inverse() * vel_world_ );
	  
	  // Filter out targets based on project target absolute speed
	  // and range limits:
	  if( std::abs( proj_speed ) < max_speed_thresh_ &&
	      t.range >= min_dist_thresh_ &&
	      t.range <= max_dist_thresh_)
	    {
	      pcl_.points.push_back( radarDataToPclPoint( t ) );
	    }
	}
      else // No velocity information available
	{
	  pcl_.points.push_back( radarDataToPclPoint( *it ) );
	}
    }

  pcl_.width = pcl_.points.size();
  pcl_.height = 1;
	
  pcl::toROSMsg( pcl_, cloud_msg_ );
  cloud_msg_.header.frame_id = msg.header.frame_id;
  cloud_msg_.header.stamp = msg.header.stamp;

  pub_pcl_.publish( cloud_msg_ );
} 

pcl::PointXYZ RadarDataVizPointCloud::radarDataToPclPoint( const radar_sensor_msgs::RadarTarget &target )
{
  pcl::PointXYZ p;
  p.x = cos( ( M_PI / 180.0 ) * target.azimuth ) * cos( ( M_PI / 180.0 ) * target.elevation )
    * target.range;
  p.y = sin( ( M_PI / 180.0 ) * target.azimuth ) * cos( ( M_PI / 180.0 ) * target.elevation )
    * target.range;
  p.z = sin( ( M_PI / 180.0 ) * target.elevation ) * target.range;

  return p;
}

double RadarDataVizPointCloud::solveForAngle( double x, double y, double z )
{
  // Solve x*cos(th)+y*sin(th)=z for th:
  //
  // th = 2*arctan((1/(x+z)) * (y+/-sqrt(y^2+x^2-z^2))) + 2*pi*n for all n
  //
  // We take n=0 and choose the minimum (abs) of the two solutions
  //
  // th_m = 2*arctan((1/(x+z)) * (y-sqrt(y^2+x^2-z^2)))
  // th_p = 2*arctan((1/(x+z)) * (y+sqrt(y^2+x^2-z^2)))
  //
  double th_p = 2.0 * atan( ( y + sqrt( y * y + x * x - z * z ) ) / ( x + z ) );
  double th_m = 2.0 * atan( ( y - sqrt( y * y + x * x - z * z ) ) / ( x + z ) );
  
  if( ( y * y + x * x ) < ( z * z ) ) // sqrt returns -nan
    {
      return 0.0;
    }
  else
    {
      return -( 180.0 / M_PI ) * ( std::abs( th_p ) < std::abs( th_m ) ? th_p : th_m ); // negative here from comparing to Yan's, not sure why needed yet...
    }
}
