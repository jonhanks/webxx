Outlining a simple HTTP server based around:
 * Boost::asio
 * Boost::beast
 * Boost::fibers
 
 Asio provides an asynchronous network abstraction, while fibers return the logic to straight line linear code.
 
 The experiment is how to use this to provide a simple interface over a scalable core.
 
 The main code is licensed under the GPL v3.
 The code under the external/asio directory is under the Boost license.
 