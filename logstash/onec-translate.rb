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
require "time"
require "yaml"

# ALT Linux doesn't ship updated gem for YAML/Psych that has support for default value
# Provides a default empty value if dictionaries are not available for read or empty
# That way, event will contain IDs and will be successfully sent to Elastic
def safe_load(path)
  count = 1

  begin
    dict = YAML.load_file(path)
  rescue
    if count < 3
      count = count + 1
      sleep(0.05)
      retry
    else
      return {}
    end
  end

  if dict.nil?
    return {}
  end

  return dict
end

def filter(event)
  db_uuid = event.get("Database")

  dict_db = safe_load("/var/lib/logstash/onec-map/dblist.yml")
  value_db = dict_db[db_uuid].nil? ? db_uuid : dict_db[db_uuid]
  event.set("Database", value_db)

  dict_user = safe_load("/var/lib/logstash/onec-map/#{db_uuid}/user.yml")
  dict_computer = safe_load("/var/lib/logstash/onec-map/#{db_uuid}/computer.yml")
  dict_application = safe_load("/var/lib/logstash/onec-map/#{db_uuid}/application.yml")
  dict_event = safe_load("/var/lib/logstash/onec-map/#{db_uuid}/event.yml")
  dict_metadata = safe_load("/var/lib/logstash/onec-map/#{db_uuid}/metadata.yml")
  dict_server = safe_load("/var/lib/logstash/onec-map/#{db_uuid}/server.yml")
  dict_primaryPort = safe_load("/var/lib/logstash/onec-map/#{db_uuid}/primary-port.yml")
  dict_secondaryPort = safe_load("/var/lib/logstash/onec-map/#{db_uuid}/secondary-port.yml")

  id_user = event.get('UserId')
  if id_user == "0"
    value_user = nil
  else
    value_user = dict_user[id_user].nil? ? id_user : dict_user[id_user]
  end
  event.set("User", value_user)

  id_computer = event.get('ComputerId')
  if id_computer == "0"
    value_computer = nil
  else
    value_computer = dict_computer[id_computer].nil? ? id_computer : dict_computer[id_computer]
  end
  event.set("Computer", value_computer)

  id_application = event.get('ApplicationId')
  if id_application == "0"
    value_application = nil
  else
    value_application = dict_application[id_application].nil? ? id_application : dict_application[id_application]
  end
  event.set("Application", value_application)

  id_event = event.get('EventId')
  if id_event == "0"
    value_event = nil
  else
    value_event = dict_event[id_event].nil? ? id_event : dict_event[id_event]
  end
  event.set("Event", value_event)

  id_metadata = event.get('MetadataId')
  if id_metadata == "0"
    value_metadata = nil
  else
    value_metadata = dict_metadata[id_metadata].nil? ? id_metadata : dict_metadata[id_metadata]
  end
  event.set("Metadata", value_metadata)

  # Multiple Metadata objects can be sent
  moreMetadata = event.get('MoreMetadata')

  array_moreMetadata = event.get('MoreMetadataArray')
  if moreMetadata == "0"
    value_moreMetadata = nil
  else
    array_moreMetadata = array_moreMetadata.split(',')
    value_moreMetadata = Array.new

    for id in array_moreMetadata do
      value_moreMetadata.push(dict_metadata[id].nil? ? id : dict_metadata[id])
    end
  end
  event.set("MoreMetadata", value_moreMetadata)

  id_server = event.get('WorkServerId')
  if id_server == "0"
    value_server = nil
  else
    value_server = dict_server[id_server].nil? ? id_server : dict_server[id_server]
  end
  event.set("WorkServer", value_server)

  id_primaryPort = event.get('PrimaryPortId')
  if id_primaryPort == "0"
    value_primaryPort = nil
  else
    value_primaryPort = dict_primaryPort[id_primaryPort].nil? ? id_primaryPort : dict_primaryPort[id_primaryPort]
  end
  event.set("PrimaryPort", value_primaryPort)

  id_secondaryPort = event.get('SecondaryPortId')
  if id_secondaryPort == "0"
    value_secondaryPort = nil
  else
    value_secondaryPort = dict_secondaryPort[id_secondaryPort].nil? ? id_secondaryPort : dict_secondaryPort[id_secondaryPort]
  end
  event.set("SecondaryPort", value_secondaryPort)

  # Transaction time is stored as seconds in hexadecimal from 01.01.0001 00:00:00 multiplied by 10000
  value_transaction = event.get('Transaction')

  if value_transaction.is_a?(String)
    value_transaction = Integer(value_transaction, 16)
  end

  if value_transaction > 0
    # Subtract seconds that represent 01.01.1970 (UNIX timestamp)
    # https://unix.stackexchange.com/a/149988
    value_transaction = value_transaction / 10000 - 62135596800
    # Hack to fix timezone
    value_transaction = Time.at(value_transaction, in: "+00:00").utc.iso8601[0..-2] + event.get("[event][timezone]")
  else
    value_transaction = nil
  end

  event.set('Transaction', value_transaction)

  # Transaction log file offset is stored as hexadecimal number
  value_transactionStartOffset = event.get('TransactionStartOffset')

  if value_transactionStartOffset.is_a?(String)
      value_transactionStartOffset = Integer(value_transactionStartOffset, 16)
  end

  if value_transactionStartOffset == 0
    value_transactionStartOffset = nil
  end

  event.set('TransactionStartOffset', value_transactionStartOffset)

  value_sessionParameters = Array.new

  dict_param = safe_load("/var/lib/logstash/onec-map/#{db_uuid}/session-parameter.yml")
  dict_param_values = safe_load("/var/lib/logstash/onec-map/#{db_uuid}/session-parameters.yml")
  sessionParameters = event.get("SessionParameters")

  if not sessionParameters.nil?
    param = sessionParameters.split(',')
    count = Integer(param[0])

    if count > 0
      (1..(count * 2)).step(2).each do |i|
        param_id = param[i]
        value_id = param[i + 1]

        if dict_param[param_id].nil?
          value_sessionParameters.push("[unknown parameter #{param_id}]")
        else
          name = dict_param[param_id]
          param_value = dict_param_values[param_id]

          if param_value[value_id].nil?
            value_sessionParameters.push("#{name}: [unknown value_id #{value_id}]")
          else
            value = param_value[value_id]
            value_sessionParameters.push("#{name} (#{value['type']}): #{value['value']}")
          end
        end

      end
    end
  end

  event.set("SessionParameters", value_sessionParameters)

  return [event]
end
