// Copyright (c) 2023 Boston Dynamics AI Institute LLC. All rights reserved.

#include <spot_driver_cpp/spot_interface.hpp>

#include <bosdyn/api/image.pb.h>
#include <builtin_interfaces/msg/time.hpp>
#include <cv_bridge/cv_bridge.h>
#include <google/protobuf/duration.pb.h>
#include <google/protobuf/timestamp.pb.h>
#include <opencv2/imgcodecs.hpp>
#include <sensor_msgs/distortion_models.hpp>
#include <sensor_msgs/msg/camera_info.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <spot_driver_cpp/spot_image_sources.hpp>
#include <spot_driver_cpp/types.hpp>
#include <std_msgs/msg/header.hpp>
#include <tl_expected/expected.hpp>

#include <iostream>
#include <stdexcept>
#include <utility>

namespace
{
tl::expected<int, std::string> getCvPixelFormat(const bosdyn::api::Image_PixelFormat& format)
{
  switch(format)
  {
    case bosdyn::api::Image_PixelFormat_PIXEL_FORMAT_RGB_U8:
    {
      return CV_8UC3;
    }
    case bosdyn::api::Image_PixelFormat_PIXEL_FORMAT_RGBA_U8:
    {
      return CV_8UC4;
    }
    case bosdyn::api::Image_PixelFormat_PIXEL_FORMAT_GREYSCALE_U8:
    {
      return CV_8UC1;
    }
    case bosdyn::api::Image_PixelFormat_PIXEL_FORMAT_GREYSCALE_U16:
    {
      return CV_16UC1;
    }
    case bosdyn::api::Image_PixelFormat_PIXEL_FORMAT_DEPTH_U16:
    {
      return CV_16UC1;
    }
    default:
    {
      return tl::make_unexpected("Unknown pixel format.");
    }
  }
}

builtin_interfaces::msg::Time applyClockSkew(const google::protobuf::Timestamp& timestamp, const google::protobuf::Duration& clock_skew)
{
  long int seconds_unskewed = timestamp.seconds() - clock_skew.seconds();
  int nanos_unskewed = timestamp.nanos() - clock_skew.nanos();

  // Carry over a second if needed
  // Note: Since ROS Time messages store the nanoseconds component as an unsigned integer, we need to do this before converting to ROS Time.
  if (nanos_unskewed < 0)
  {
    nanos_unskewed += 1e9;
    seconds_unskewed -= 1;
  }

  // If the timestamp contains a negative time, create an all-zero ROS Time.
  if (seconds_unskewed < 0)
  {
    return builtin_interfaces::build<builtin_interfaces::msg::Time>().sec(0).nanosec(0);
  }
  else
  {
    return builtin_interfaces::build<builtin_interfaces::msg::Time>().sec(seconds_unskewed).nanosec(nanos_unskewed);
  }
}

tl::expected<sensor_msgs::msg::CameraInfo, std::string> toCameraInfoMsg(const bosdyn::api::ImageResponse& image_response, const google::protobuf::Duration& clock_skew)
{
  sensor_msgs::msg::CameraInfo info_msg;
  info_msg.distortion_model = sensor_msgs::distortion_models::PLUMB_BOB;
  info_msg.height = image_response.shot().image().rows();
  info_msg.width = image_response.shot().image().cols();
  // TODO: use Spot's prefix here
  info_msg.header.frame_id = image_response.shot().frame_name_image_sensor();
  info_msg.header.stamp = applyClockSkew(image_response.shot().acquisition_time(), clock_skew);

  // We assume that the camera images have already been corrected for distortion, so the 5 distortion parameters are all zero
  info_msg.d = std::vector<double>{5, 0.0};

  // Set the rectification matrix to identity, since this is not a stereo pair.
  info_msg.r[0] = 1.0;
  info_msg.r[1] = 0.0;
  info_msg.r[2] = 0.0;
  info_msg.r[3] = 0.0;
  info_msg.r[4] = 1.0;
  info_msg.r[5] = 0.0;
  info_msg.r[6] = 0.0;
  info_msg.r[7] = 0.0;
  info_msg.r[8] = 1.0;

  const auto& intrinsics = image_response.source().pinhole().intrinsics();

  // Create the 3x3 intrinsics matrix.
  info_msg.k[0] = intrinsics.focal_length().x();
  info_msg.k[2] = intrinsics.principal_point().x();
  info_msg.k[4] = intrinsics.focal_length().y();
  info_msg.k[5] = intrinsics.principal_point().y();
  info_msg.k[8] = 1.0;

  // All Spot cameras are functionally monocular, so Tx and Ty are not set here.
  info_msg.p[0] = intrinsics.focal_length().x();
  info_msg.p[2] = intrinsics.principal_point().x();
  info_msg.p[5] = intrinsics.focal_length().y();
  info_msg.p[6] = intrinsics.principal_point().y();
  info_msg.p[10] = 1.0;

  return info_msg;
}

tl::expected<sensor_msgs::msg::Image, std::string> toImageMsg(const bosdyn::api::ImageCapture& image_capture, const google::protobuf::Duration& clock_skew)
{
      const auto& image = image_capture.image();
      auto data = image.data();

      std_msgs::msg::Header header;
      // TODO: use Spot's prefix here
      header.frame_id = image_capture.frame_name_image_sensor();
      header.stamp = applyClockSkew(image_capture.acquisition_time(), clock_skew);

      const auto pixel_format_cv = getCvPixelFormat(image.pixel_format());
      if (!pixel_format_cv)
      {
        return tl::make_unexpected("Failed to convert image to message: " + pixel_format_cv.error());
      }

      if (image.format() == bosdyn::api::Image_Format_FORMAT_JPEG)
      {
        // When the image is JPEG-compressed, it is represented as a 1 x (width * height) row of bytes.
        // First we create a cv::Mat which contains the compressed image data...
        const cv::Mat img_compressed{ 1, image.rows() * image.cols(), CV_8UC1, &data.front()};
        // Then we decode it to extract the raw image into a new cv::Mat.
        // Note: this assumes that if an image is provided as JPEG-compressed data, then it is an RGB image.
        const cv::Mat img_bgr = cv::imdecode(img_compressed, cv::IMREAD_COLOR);
        if (!img_bgr.data)
        {
          return tl::make_unexpected("Failed to decode JPEG-compressed image.");
        }
        const auto image = cv_bridge::CvImage{header, "bgr8", img_bgr}.toImageMsg();
        return *image;
      }
      else if (image.format() == bosdyn::api::Image_Format_FORMAT_RAW)
      {
        // Note: as currently implemented, this assumes that the only images which will be provided as raw data will be 16UC1 depth images.
        // TODO: handle converting raw RGB and grayscale images as well.
        const cv::Mat img = cv::Mat(image.rows(), image.cols(), pixel_format_cv.value(), &data.front());
        const auto image = cv_bridge::CvImage{header, "mono16", img}.toImageMsg();
        return *image;
      }
      else if (image.format() == bosdyn::api::Image_Format_FORMAT_RLE)
      {
        return tl::make_unexpected("Conversion from FORMAT_RLE is not yet implemented.");
      }
      else
      {
        return tl::make_unexpected("Unknown image format.");
      }
}
}

namespace spot_ros2
{
SpotInterface::SpotInterface()
: client_sdk_{ ::bosdyn::client::CreateStandardSDK("get_image") }
{
}

bool SpotInterface::createRobot(const std::string& ip_address)
{
  auto create_robot_result = client_sdk_->CreateRobot(ip_address);
  if(!create_robot_result.status)
  {
    return false;
  }

  robot_ = std::move(create_robot_result.response);

  return true;
}

bool SpotInterface::authenticate(const std::string& username, const std::string& password)
{
  if (!robot_)
  {
    return false;
  }

  const auto authenticate_result = robot_->Authenticate(username, password);
  if(!authenticate_result)
  {
    return false;
  }

  const auto start_time_sync_response = robot_->StartTimeSync();
  if (!start_time_sync_response)
  {
    return false;
  }

  const auto get_time_sync_thread_response = robot_->GetTimeSyncThread();
  if (!get_time_sync_thread_response)
  {
    return false;
  }
  time_sync_thread_ = get_time_sync_thread_response.response;

  const auto image_client_result =
        robot_->EnsureServiceClient<::bosdyn::client::ImageClient>(
            ::bosdyn::client::ImageClient::GetDefaultServiceName());
  if (!image_client_result.status)
  {
    return false;
  }

  image_client_.reset(std::move(image_client_result.response));

  return true;
}

bool SpotInterface::hasArm() const
{
  // TODO: programmatically determine if Spot has an arm attached, like the existing Python driver does
  return true;
}

tl::expected<GetImagesResult, std::string> SpotInterface::getImages(::bosdyn::api::GetImageRequest request)
{
  std::shared_future<::bosdyn::client::GetImageResultType> get_image_result_future = image_client_->GetImageAsync(request);

  ::bosdyn::client::GetImageResultType get_image_result = get_image_result_future.get();
  if (!get_image_result.status) {
      return tl::make_unexpected("Failed to get images: " + get_image_result.status.DebugString());
  }

  const auto clock_skew_result = getClockSkew();
  if (!clock_skew_result)
  {
    return tl::make_unexpected("Failed to get latest clock skew: " + clock_skew_result.error());
  }

  GetImagesResult out;
  for (const auto& image_response : get_image_result.response.image_responses())
  {      
      const auto& image = image_response.shot().image();
      auto data = image.data();

      const auto image_msg = toImageMsg(image_response.shot(), clock_skew_result.value());
      if (!image_msg)
      {
        std::cerr << "Failed to convert SDK image response to ROS Image message: " << image_msg.error() << std::endl;
        continue;
      }

      const auto info_msg = toCameraInfoMsg(image_response, clock_skew_result.value());
      if (!info_msg)
      {
        std::cerr << "Failed to convert SDK image response to ROS CameraInfo message: " << info_msg.error() << std::endl;
        continue;
      }

      const auto& camera_name = image_response.source().name();
      if(const auto result = fromSpotImageSourceName(camera_name); result.has_value())
      {
        out.try_emplace(result.value(), ImageWithCameraInfo{image_msg.value(), info_msg.value()});
      }
      else
      {
        std::cerr << "Failed to convert API image source name to ImageSource: " << result.error() << std::endl;
      }
  }

  return out;
}

tl::expected<builtin_interfaces::msg::Time, std::string> SpotInterface::convertRobotTimeToLocalTime(const google::protobuf::Timestamp& robot_timestamp)
{
  const auto get_clock_skew_result = getClockSkew();
  if (!get_clock_skew_result)
  {
    return tl::make_unexpected("Failed to get clock skew: " + get_clock_skew_result.error());
  }

  return applyClockSkew(robot_timestamp, get_clock_skew_result.value());
}


tl::expected<google::protobuf::Duration, std::string> SpotInterface::getClockSkew()
{
  const auto get_skew_response = time_sync_thread_->GetEndpoint()->GetClockSkew();
  if (!get_skew_response)
  {
    return tl::make_unexpected("Failed to get clock skew: " + get_skew_response.status.DebugString());
  }
  return *get_skew_response.response;
}
}
