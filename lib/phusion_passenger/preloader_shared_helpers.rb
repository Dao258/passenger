# encoding: binary
#  Phusion Passenger - https://www.phusionpassenger.com/
#  Copyright (c) 2011, 2012 Phusion
#
#  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
#
#  See LICENSE file for license information.

require 'socket'
require 'phusion_passenger/native_support'

module PhusionPassenger

# Provides shared functions for preloader apps.
module PreloaderSharedHelpers
	extend self

	def init
		if !Kernel.respond_to?(:fork)
			message = "Smart spawning is not available on this Ruby " +
				"implementation because it does not support `Kernel.fork`. "
			if ENV['SERVER_SOFTWARE'].to_s =~ /nginx/i
				message << "Please set `passenger_spawn_method` to `direct`."
			else
				message << "Please set `PassengerSpawnMethod` to `direct`."
			end
			raise(message)
		end
	end
	
	def accept_and_process_next_client(server_socket)
		original_pid = Process.pid
		client = server_socket.accept
		client.binmode
		begin
			command = client.readline
		rescue EOFError
			return nil
		end
		if command !~ /\n\Z/
			STDERR.puts "Command must end with a newline"
		elsif command == "spawn\n"
			while client.readline != "\n"
				# Do nothing.
			end
			
			# Improve copy-on-write friendliness.
			GC.start
			
			pid = fork
			if pid.nil?
				$0 = "#{$0} (forking...)"
				client.puts "OK"
				client.puts Process.pid
				client.flush
				client.sync = true
				return [:forked, client]
			else
				NativeSupport.detach_process(pid)
			end
		else
			STDERR.puts "Unknown command '#{command.inspect}'"
		end
		return nil
	ensure
		client.close if client && Process.pid == original_pid
	end
	
	def run_main_loop(options)
		$0 = "Passenger AppPreloader: #{options['app_root']}"
		client = nil
		original_pid = Process.pid
		socket_filename = "#{options['generation_dir']}/backends/preloader.#{Process.pid}"
		server = UNIXServer.new(socket_filename)
		server.close_on_exec!
		
		# Update the dump information just before telling the preloader that we're
		# ready because the HelperAgent will read and memorize this information.
		LoaderSharedHelpers.dump_all_information

		puts "!> Ready"
		puts "!> socket: unix:#{socket_filename}"
		puts "!> "
		
		while true
			ios = select([server, STDIN])[0]
			if ios.include?(server)
				result, client = accept_and_process_next_client(server)
				if result == :forked
					STDIN.reopen(client)
					STDOUT.reopen(client)
					STDOUT.sync = true
					client.close
					return :forked
				end
			end
			if ios.include?(STDIN)
				if STDIN.tty?
					begin
						# Prevent bash from exiting when we press Ctrl-D.
						STDIN.read_nonblock(1)
					rescue Errno::EAGAIN
						# Do nothing.
					end
				end
				break
			end
		end
		return nil
	ensure
		server.close if server
		if original_pid == Process.pid
			File.unlink(socket_filename) rescue nil
		end
	end
end

end # module PhusionPassenger
