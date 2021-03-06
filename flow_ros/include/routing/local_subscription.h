/**
 * @copyright 2020 Fetch Robotics Inc. All rights reserved
 * @author Brian Cairl
 *
 * @file local_subscription.h
 */
#ifndef FLOW_ROS_ROUTING_LOCAL_SUBSCRIPTION_H
#define FLOW_ROS_ROUTING_LOCAL_SUBSCRIPTION_H

// C++ Standard Library
#include <cstdint>
#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

// ROS Bag
#include <rosbag/message_instance.h>

// Flow
#include <flow_ros/routing/subscription_wrapper.h>

namespace flow_ros
{
namespace routing
{

/**
 * @brief Exception thrown when a message cannot be instanced from <code>rosbag::MessageInstance</code>
 */
class MessageInstanceError final : public std::exception
{
public:
  template <typename StringT> explicit MessageInstanceError(StringT&& what) : what_{std::forward<StringT>(what)} {}

  const char* what() const noexcept { return what_.c_str(); }

private:
  std::string what_;
};


/**
 * @brief Exception thrown when a message type does not match for a given topic
 */
class MessageTypeError final : public std::exception
{
public:
  template <typename StringT> explicit MessageTypeError(StringT&& what) : what_{std::forward<StringT>(what)} {}

  const char* what() const noexcept { return what_.c_str(); }

private:
  std::string what_;
};


/**
 * @brief LocalSubscription base which provides basic info and erases MsgT-dependent methods
 */
class LocalSubscriptionBase
{
public:
  virtual ~LocalSubscriptionBase() = default;

  LocalSubscriptionBase() = default;

  /**
   * @brief Calls subscriber callback with a rosbag message instance
   */
  virtual void inject(const ::rosbag::MessageInstance& mi) const = 0;
};


template <typename MsgT> class LocalSubscription final : public LocalSubscriptionBase, public SubscriptionWrapper
{
public:
  /**
   * @brief Required setup constructor
   *
   * @param topic  topic name to associate with this object
   * @param cb  subscriber callback to associate with this object
   */
  template <typename CallbackT>
  LocalSubscription(std::string topic, CallbackT&& cb) :
      topic_{std::move(topic)},
      callback_{std::forward<CallbackT>(cb)}
  {}

  /**
   * @brief Deconstructor
   */
  virtual ~LocalSubscription() = default;

  /**
   * @brief Calls subscriber callback with a new message
   *
   * @param message  message data
   */
  inline void call(const message_shared_const_ptr_t<MsgT>& message) const { callback_(message); }

  /**
   * @brief Calls subscriber callback with a rosbag message instance
   *
   * @param mi  ROS bag message instance
   *
   * @throws <code>std::runtime_error</code> on failure to instance message
   * @note participates in overload resolution if <code>MsgT</code> is a ROS message
   */
  void inject(const ::rosbag::MessageInstance& mi) const override { call_impl(mi); }

  /**
   * @copydoc SubscriptionWrapper::getTopic
   */
  std::string getTopic() const override { return topic_; }

  /**
   * @copydoc SubscriptionWrapper::getNumPublishers
   */
  std::uint32_t getNumPublishers() const override { return 0; /*unknown*/ }

  /**
   * @copydoc SubscriptionWrapper::getTransportMethod
   */
  TransportMethod getTransportMethod() const override { return TransportMethod::LOCAL; }

  /**
   * @copydoc SubscriptionWrapper::isValid
   *
   * @retval true  if a callback has been registered to this object
   * @retval false  otherwise
   */
  bool isValid() const override { return static_cast<bool>(callback_); }

private:
  /**
   * @brief Calls subscriber callback with a rosbag message instance
   *
   * @param mi  ROS bag message instance
   *
   * @throws <code>MessageInstanceError</code> on failure to instance message
   *
   * @note participates in overload resolution if <code>MsgT</code> is a ROS message
   */
  template <bool U = ros::message_traits::IsMessage<MsgT>::value>
  inline std::enable_if_t<U> call_impl(const ::rosbag::MessageInstance& mi) const
  {
    const auto msg = mi.template instantiate<std::remove_const_t<MsgT>>();

    if (msg)
    {
      callback_(msg);
    }
    else
    {
      std::ostringstream oss;
      oss << "Could not instance message on topic: " << mi.getTopic() << "(md5sum=" << mi.getMD5Sum() << ')';
      throw MessageInstanceError{oss.str()};
    }
  }

  /**
   * @throws <code>MessageInstanceError</code> always
   *
   * @note participates in overload resolution if <code>MsgT</code> is NOT a ROS message
   */
  template <bool U = ros::message_traits::IsMessage<MsgT>::value>
  inline std::enable_if_t<!U> call_impl(const ::rosbag::MessageInstance&) const
  {
    throw MessageInstanceError{"Message type is not a ROS message"};
  }

  /// Required callback type alias
  using CallbackType = std::function<void(const message_shared_const_ptr_t<MsgT>&)>;

  /// Topic name
  std::string topic_;

  /// Associated subscriber callback
  CallbackType callback_;
};

/**
 * @brief Holds a group of LocalSubscription objects associated with a particular topic
 */
class LocalSubscriptionGroup
{
public:
  /**
   * @brief Calls all subscriber callbacks on a new message
   *
   * @param message  message data
   *
   * @throws <code>MessageTypeError</code> if <code>MsgT</code> is incompatible with subscription group
   */
  template <typename MsgT> inline void call(const message_shared_const_ptr_t<MsgT>& message) const
  {
    // All subscriptions are local subscriptions for the same message type
    for (auto it = members_.begin(); it != members_.end(); /*empty*/)
    {
      const auto sub = it->lock();
      if (!static_cast<bool>(sub))
      {
        // Subscriber has expired, remove from list
        it = members_.erase(it);
        continue;
      }

      const auto local_sub = std::dynamic_pointer_cast<LocalSubscription<std::add_const_t<MsgT>>>(sub);
      if (static_cast<bool>(local_sub))
      {
        local_sub->call(message);
        ++it;
      }
      else
      {
        throw MessageTypeError{"Invalid message type for subscription"};
      }
    }
  }

  /**
   * @brief Calls subscriber callback with a rosbag message instance
   *
   * @param mi  ROS bag message instance
   */
  inline void inject(const ::rosbag::MessageInstance& mi) const
  {
    for (auto it = members_.begin(); it != members_.end(); /*empty*/)
    {
      const auto sub = it->lock();
      if (static_cast<bool>(sub))
      {
        ++it;
        sub->inject(mi);
      }
      else
      {
        // Subscriber has expired, remove from list
        it = members_.erase(it);
      }
    }
  }

  /**
   * @brief Adds new subscriptions to group
   *
   * @param sub  LocalSubscription resource
   */
  template <typename MsgT> inline void addSubscription(std::shared_ptr<LocalSubscription<MsgT>> sub)
  {
    // Lock before adding subscription to block injection
    members_.emplace_back(std::move(sub));
  }

  /**
   * @brief Validation cast operator
   *
   * @retval true  if there are any held subscriptions
   * @retval false  otherwise
   */
  inline operator bool() const { return !static_cast<bool>(members_.empty()); }

  /**
   * @brief Returns the number of held subscriptions
   */
  inline std::size_t size() const { return members_.size(); }

private:
  /// Held subscriptions associated with group
  mutable std::vector<std::weak_ptr<LocalSubscriptionBase>> members_;
};

}  // namespace routing
}  // namespace flow_ros


#endif  // FLOW_ROS_ROUTING_LOCAL_SUBSCRIPTION_H
