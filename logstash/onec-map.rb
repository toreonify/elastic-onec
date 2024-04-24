#
# This file is part of the elastic-onec (https://github.com/toreonify/elastic-onec).
# Copyright (c) 2024 Ivan Korytov <toreonify@outlook.com>.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, version 3.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.
#
require "yaml"
require "fileutils"

def filter(_event)
  db_uuid = _event.get("Database")
  message = _event.get("message")

  FileUtils.mkdir_p "/var/lib/logstash/onec-map/#{db_uuid}/"

  user = Hash.new
  computer = Hash.new
  application = Hash.new
  event = Hash.new
  metadata = Hash.new
  server = Hash.new
  primary_port = Hash.new
  secondary_port = Hash.new
  session_parameter = Hash.new
  session_parameters = Hash.new

  previous_type = nil
  message.each_line do |line|
    values = line.scan /^{([0-9]+),/

    type = "unknown"
    if not values.empty?
      type = values[0][0]
    end

    case type
    when "1"
      # User
      values = line.scan /^{([0-9]+),(.*),"(.*)",([0-9]+)},?$/

      if not values.empty?
        id = values[0][3]
        name = values[0][2]
        user[id] = name
      end
    when "2"
      # Computer
      values = line.scan /^{([0-9]+),"(.*)",([0-9]+)},?$/

      if not values.empty?
        id = values[0][2]
        name = values[0][1]
        computer[id] = name
      end
    when "3"
      # Application
      values = line.scan /^{([0-9]+),"(.*)",([0-9]+)},?$/

      if not values.empty?
        id = values[0][2]
        name = values[0][1]
        application[id] = name
      end
    when "4"
      # Event
      values = line.scan /^{([0-9]+),"(.*)",([0-9]+)},?$/

      if not values.empty?
        id = values[0][2]
        desc = values[0][1]
        event[id] = desc
      end
    when "5"
      # Metadata
      values = line.scan /^{([0-9]+),(.*),"(.*)",([0-9]+)},?$/

      if not values.empty?
        id = values[0][3]
        desc = values[0][2]
        metadata[id] = desc
      end
    when "6"
      # Server
      values = line.scan /^{([0-9]+),"(.*)",([0-9]+)},?$/

      if not values.empty?
        id = values[0][2]
        name = values[0][1]
        server[id] = name
      end
    when "7"
      # Primary port
      values = line.scan /^{([0-9]+),([0-9]+),([0-9]+)},?$/

      if not values.empty?
        id = values[0][2]
        port = values[0][1]
        primary_port[id] = port
      end
    when "8"
      # Secondary port
      values = line.scan /^{([0-9]+),([0-9]+),([0-9]+)},?$/

      if not values.empty?
        id = values[0][2]
        port = values[0][1]
        secondary_port[id] = port
      end
    when "9"
      # Session parameter name declaration; data partitioning
      values = line.scan /^{([0-9]+),(.*),"(.*)",([0-9]+)},?$/

      if not values.empty?
        id = values[0][3]
        name = values[0][2]
        session_parameter[id] = name
      end

    when "10"
      # Skip
    when "11"
      # Skip
    when "12"
      # Skip

    when "13"
      # Computer and user bind (computer id, user id)
    else
      # whoops
      puts "Unknown type: #{type}"
    end

    # Two-line properties
    case previous_type
      when "10"
        # Session parameter value
        values = line.scan /^{"(.*)","?(.*)"?},([0-9]+),([0-9]+)},?$/
        # Guessed: value_type, value, prop_id, value_id
        # Last value may be a server_id, but I do not have more data to test
        # But more likely it is a value id for a parameter, because they are the same in my test data
        if not values.empty?
          value_type = values[0][0]
          value = values[0][1]
          id = values[0][2]
          value_id = values[0][3]

          if session_parameters[id].nil?
            session_parameters[id] = {}
          end

          if session_parameters[id][value_id].nil?
            session_parameters[id][value_id] = {}
          end

          session_parameters[id][value_id]["type"] = value_type
          session_parameters[id][value_id]["value"] = value
        end
      when "11"
        # Session parameters bind to user on login
      when "12"
        # Session parameters bind to client computer on login
    end

    previous_type = type
  end

  File.write("/var/lib/logstash/onec-map/#{db_uuid}/user.yml", user.to_yaml, mode: "w")
  File.write("/var/lib/logstash/onec-map/#{db_uuid}/computer.yml", computer.to_yaml, mode: "w")
  File.write("/var/lib/logstash/onec-map/#{db_uuid}/application.yml", application.to_yaml, mode: "w")
  File.write("/var/lib/logstash/onec-map/#{db_uuid}/event.yml", event.to_yaml, mode: "w")
  File.write("/var/lib/logstash/onec-map/#{db_uuid}/metadata.yml", metadata.to_yaml, mode: "w")
  File.write("/var/lib/logstash/onec-map/#{db_uuid}/server.yml", server.to_yaml, mode: "w")
  File.write("/var/lib/logstash/onec-map/#{db_uuid}/primary-port.yml", primary_port.to_yaml, mode: "w")
  File.write("/var/lib/logstash/onec-map/#{db_uuid}/secondary-port.yml", secondary_port.to_yaml, mode: "w")
  File.write("/var/lib/logstash/onec-map/#{db_uuid}/session-parameter.yml", session_parameter.to_yaml, mode: "w")
  File.write("/var/lib/logstash/onec-map/#{db_uuid}/session-parameters.yml", session_parameters.to_yaml, mode: "w")

  return [_event]
end
