////////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2020 NovAtel Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
////////////////////////////////////////////////////////////////////////////////

#include <ros/ros.h>
#include <nodelet/nodelet.h>
#include <ros/callback_queue.h>

#include <boost/thread/mutex.hpp>
#include <boost/asio.hpp>
#include <boost/interprocess/sync/interprocess_semaphore.hpp>
#include <map>

#include "novatel_oem7_msgs/Oem7AbasciiCmd.h"
#include "novatel_oem7_msgs/Oem7RawMsg.h"


#include <pluginlib/class_loader.h>



#include <novatel_oem7_driver/oem7_receiver_if.hpp>
#include <novatel_oem7_driver/oem7_message_decoder_if.hpp>

#include <novatel_oem7_driver/oem7_message_util.hpp>
#include <novatel_oem7_driver/ros_messages.hpp>
#include <oem7_ros_publisher.hpp>

#include <message_handler.hpp>


namespace novatel_oem7_driver
{
  /**
   * Nodelet publishing raw oem7 messages and issuing oem7 commands.
   * Loads plugins responsible for obtaining byte input from the Oem7 receiver, and decoding it into raw oem7 messages.
   * Implements a service allowing Oem7 Abbreviated ASCII commands to be sent to the receiver.
   */
  class Oem7MessageNodelet :
      public Oem7MessageDecoderUserIf,
      public nodelet::Nodelet
  {
    boost::mutex nodelet_mtx_; ///< Protects nodelet internal state

    double publish_delay_sec_; ///< Delay after publishing each message; used to throttle output with static data sources.

    Oem7RosPublisher oem7rawmsg_pub_; ///< Publishes raw Oem7 messages.

    ros::CallbackQueue timer_queue_; ///< Dedicated queue for command requests.
    boost::shared_ptr<ros::AsyncSpinner> timer_spinner_; ///< 1 thread servicing the command queue.

    // Command service
    boost::interprocess::interprocess_semaphore rsp_sem_; ///< Signaling between Oem7 commands / response handler
    std::string rsp_; ///< The latest response from Oem7 receiver.
    ros::CallbackQueue queue_; //< Dedicated queue for command requests.
    boost::shared_ptr<ros::AsyncSpinner> aspinner_; ///< 1 thread servicing the command queue.
    ros::ServiceServer oem7_cmd_srv_; ///< Oem7 command service.


    ros::Timer timer_; ///< One time service callback.

    pluginlib::ClassLoader<novatel_oem7_driver::Oem7ReceiverIf> recvr_loader_;
    pluginlib::ClassLoader<novatel_oem7_driver::Oem7MessageDecoderIf> oem7_msg_decoder_loader;

    std::set<int> raw_msg_pub_; ///< Set of raw messages to publish.

    boost::shared_ptr<MessageHandler> msg_handler_; ///< Dispatches individual messages for handling.

    // Log statistics
    long total_log_count_; ///< Total number of logs received

    typedef std::map<int, long> log_count_map_t;
    log_count_map_t log_counts_; ///< Indidividual log counts.
    long discarded_msg_num_; ///< Number of messages received and discarded by the driver.


    boost::shared_ptr<novatel_oem7_driver::Oem7MessageDecoderIf> msg_decoder; ///< Message Decoder plugin
    boost::shared_ptr<novatel_oem7_driver::Oem7ReceiverIf> recvr_; ///< Oem7 Receiver Interface plugin



    /**
     * Wraps actual initializating for exception handling.
     */
    void onInit()
    {
      try
      {
        onInitImpl();
      }
      catch(std::exception  const& ex)
      {
        NODELET_FATAL_STREAM("Fatal: " << ex.what());

        ros::shutdown();
      }
    }

    /**
     * Loads plugins, sets up threading/callbacks, advertises messages and services.
     */
    void onInitImpl()
    {
      NODELET_INFO_STREAM(getName() << ": Oem7MessageNodelet v." << novatel_oem7_driver_VERSION << "; "
                                                                 << __DATE__ << " " << __TIME__);

      boost::lock_guard<boost::mutex> guard(nodelet_mtx_);


      initializeOem7MessageUtil(getNodeHandle());

      getNodeHandle().setCallbackQueue(&timer_queue_);

      getPrivateNodeHandle().getParam("oem7_publish_delay", publish_delay_sec_);
      if(publish_delay_sec_ > 0)
      {
        NODELET_WARN_STREAM("Publish Delay: " << publish_delay_sec_ << " seconds. Is this is a test?");
      }
      // Load plugins

      // Load Oem7Receiver
      std::string oem7_if_name;
      getPrivateNodeHandle().getParam("oem7_if", oem7_if_name);
      recvr_ = recvr_loader_.createInstance(oem7_if_name);
      recvr_->initialize(getPrivateNodeHandle());

      // Load Oem7 Message Decoder
      std::string msg_decoder_name;
      getPrivateNodeHandle().getParam("oem7_msg_decoder", msg_decoder_name);
      msg_decoder = oem7_msg_decoder_loader.createInstance(msg_decoder_name);
      msg_decoder->initialize(getPrivateNodeHandle(), recvr_.get(), this);


      msg_handler_.reset(new MessageHandler(getPrivateNodeHandle()));

      // Oem7 raw messages to publish.
      std::vector<std::string> oem7_raw_msgs;
      bool ok = getPrivateNodeHandle().getParam("oem7_raw_msgs", oem7_raw_msgs);
      for(const auto& msg : oem7_raw_msgs)
      {
        int raw_msg_id = getOem7MessageId(msg);
        if(raw_msg_id == 0)
        {
          NODELET_ERROR_STREAM("Unknown Oem7 message '" << msg );
        }
        else
        {
          NODELET_INFO_STREAM("Oem7 Raw message '" << msg << "' will be published." );
          raw_msg_pub_.insert(raw_msg_id);
        }
      }

      oem7rawmsg_pub_.setup<novatel_oem7_msgs::Oem7RawMsg>("Oem7RawMsg", getPrivateNodeHandle());

      timer_spinner_.reset(new ros::AsyncSpinner(1, &timer_queue_)); //< 1 thread servicing the command queue.
      timer_spinner_->start();

      timer_ =  getNodeHandle().createTimer(ros::Duration(0.0), &Oem7MessageNodelet::serviceLoopCb, this, true);

      aspinner_.reset(new ros::AsyncSpinner(1, &queue_));
      aspinner_->start();

      ros::AdvertiseServiceOptions ops = ros::AdvertiseServiceOptions::create<novatel_oem7_msgs::Oem7AbasciiCmd>(
                                                            "Oem7Cmd",
                                                            boost::bind(&Oem7MessageNodelet::serviceOem7AbasciiCb, this, _1, _2),
                                                            ros::VoidConstPtr(),
                                                            &queue_);
      oem7_cmd_srv_ = getPrivateNodeHandle().advertiseService(ops);
    }


    /**
     * Called to request O7AbasciiCmd service
     */
    bool serviceOem7AbasciiCb(novatel_oem7_msgs::Oem7AbasciiCmd::Request& req, novatel_oem7_msgs::Oem7AbasciiCmd::Response& rsp)
    {
      NODELET_DEBUG_STREAM("AACmd: cmd '" << req.cmd << "'");

      // Protect this with semaphore vs race with init code
      nodelet_mtx_.lock();
      rsp_.clear();
      nodelet_mtx_.unlock();

      for(int attempt = 0;
              attempt < 10;
              attempt++)
      {
         recvr_->write(boost::asio::buffer(req.cmd));

	 static const std::string NEWLINE("\n");
	 recvr_->write(boost::asio::buffer(NEWLINE));
 
         boost::system_time const timeout = boost::get_system_time() + boost::posix_time::milliseconds(3 * 1000);
	 if(rsp_sem_.timed_wait(timeout))
         {
            break;
         }
         NODELET_ERROR_STREAM("Attempt " << attempt << ": timed out waiting for response.");
      }

      nodelet_mtx_.lock();
      rsp.rsp = rsp_;
      rsp_.clear();
      nodelet_mtx_.unlock();

      if(rsp.rsp == "OK")
      {
        NODELET_INFO_STREAM("AACmd '" << req.cmd << "' : " << "'" << rsp.rsp << "'");
      }
      else
      {
        NODELET_ERROR_STREAM("AACmd '" << req.cmd << "' : " << "'" << rsp.rsp << "'");
      }

      return true;
    }

    /**
     * Outputs Log statistics to ROS console.
     */
    void outputLogStatistics()
    {
      NODELET_INFO("Log Statistics:");
      NODELET_INFO_STREAM("Logs: " << total_log_count_ << "; discarded: " << discarded_msg_num_);

      for(log_count_map_t::iterator itr = log_counts_.begin();
                                    itr != log_counts_.end();
                                    itr++)
      {
        int  id    = itr->first;
        long count = itr->second;

        NODELET_INFO_STREAM("Log[" << getOem7MessageName(id) << "](" << id << "):" <<  count);
      }
    }

    /*
     * Update Log statistics for a particular message
     */
    void updateLogStatistics(const Oem7RawMessageIf::ConstPtr& raw_msg)
    {
      total_log_count_++;

      if(log_counts_.find(raw_msg->getMessageId()) == log_counts_.end())
      {
        log_counts_[raw_msg->getMessageId()] = 0;
      }
      log_counts_[raw_msg->getMessageId()]++;


      if((total_log_count_ % 10000) == 0)
      {
        outputLogStatistics();
      }
    }

    /**
     * Called by ROS decoder with new raw messages
     */
    void onNewMessage(Oem7RawMessageIf::ConstPtr raw_msg)
    {


      NODELET_DEBUG_STREAM("onNewMsg: fmt= " << raw_msg->getMessageFormat()
                              <<   " type= " << raw_msg->getMessageType());

      // Discard all unknown messages; this is normally when dealing with responses to ASCII commands.
      if(raw_msg->getMessageFormat() == Oem7RawMessageIf::OEM7MSGFMT_UNKNOWN)
      {
        ++discarded_msg_num_;
        NODELET_DEBUG_STREAM("Discarded:  [ID: " << raw_msg->getMessageType()       <<
                                          " T: " << raw_msg->getMessageType()       <<
                                          " F: " << raw_msg->getMessageFormat()     <<
                                          " L: " << raw_msg->getMessageDataLength() <<
                                          "]");
      }
      else
      {
        if(raw_msg->getMessageFormat() == Oem7RawMessageIf::OEM7MSGFMT_ASCII  ||
           raw_msg->getMessageFormat() == Oem7RawMessageIf::OEM7MSGFMT_ABASCII)
        {
          std::string msg(raw_msg->getMessageData(0), raw_msg->getMessageData(raw_msg->getMessageDataLength()));

          NODELET_DEBUG_STREAM(">---------------------------" << std::endl
                            << msg                            << std::endl
                            << "<---------------------------");
        }

        if(raw_msg->getMessageType() == Oem7RawMessageIf::OEM7MSGTYPE_RSP) // Response
        {
          std::string rsp(raw_msg->getMessageData(0), raw_msg->getMessageData(raw_msg->getMessageDataLength()));


          nodelet_mtx_.lock();
          rsp_ = rsp;
          nodelet_mtx_.unlock();

          rsp_sem_.post();
        }
        else // Log
        {
          if(raw_msg->getMessageFormat() == Oem7RawMessageIf::OEM7MSGFMT_BINARY) // binary
          {
            updateLogStatistics(raw_msg);

            msg_handler_->handleMessage(raw_msg);


            if(raw_msg_pub_.find(raw_msg->getMessageId()) != raw_msg_pub_.end())
            {
              novatel_oem7_msgs::Oem7RawMsg::Ptr oem7_raw_msg(new novatel_oem7_msgs::Oem7RawMsg);
              oem7_raw_msg->message_data.insert(
                                            oem7_raw_msg->message_data.end(),
                                             raw_msg->getMessageData(0),
                                             raw_msg->getMessageData(raw_msg->getMessageDataLength()));

              assert(oem7_raw_msg->message_data.size() == raw_msg->getMessageDataLength());

              oem7rawmsg_pub_.publish(oem7_raw_msg);
            }

            if(publish_delay_sec_ > 0)
            {
              sleep(publish_delay_sec_);
            }
          }
        }
      }
    }


    /**
     * Service loop; drives Oem7 message decoder. onNewMessage is called on this thread.
     */
    void serviceLoopCb(const ros::TimerEvent& event)
    {
      msg_decoder->service();

      outputLogStatistics();

      NODELET_WARN("No more input from Decoder; Oem7MessageNodelet finished.");
    }


  public:
    Oem7MessageNodelet():
      recvr_loader_(          "novatel_oem7_driver", "novatel_oem7_driver::Oem7ReceiverIf"),
      oem7_msg_decoder_loader("novatel_oem7_driver", "novatel_oem7_driver::Oem7MessageDecoderIf"),
      rsp_sem_(0),
      total_log_count_(0),
      discarded_msg_num_(0),
      publish_delay_sec_(0)
    {
    }

    ~Oem7MessageNodelet()
    {
      NODELET_DEBUG("~Oem7MessageNodelet");
    }
  };
}


#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(novatel_oem7_driver::Oem7MessageNodelet, nodelet::Nodelet)