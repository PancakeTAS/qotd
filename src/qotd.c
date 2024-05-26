/**
 * Copyright (C) 2024  Pancake
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <signal.h>
#include <string.h>

#include <concord/discord.h>
#include <concord/log.h>

/// Config file for the bot
#define CONFIG_FILE "config.json"

/// Lumi's user ID
#define LUMI_USER_ID 905564480082153543ULL
/// QOTD role ID
#define QOTD_ROLE_ID 1215105959815553096ULL
/// QOTD channel ID
#define QOTD_CHANNEL_ID 1198507295538151434ULL

/**
 * Handle SIGINT signal and shut down the bot
 *
 * \param signum Signal number
 */
static void handle_sigint(int signum) {
    log_info("[QOTD] Received SIGINT, shutting down bot...");
    ccord_shutdown_async();
}

/**
 * Initialize discord client
 *
 * \return Discord client on success, NULL on failure
 */
static struct discord* initialize_discord() {
    // initialize concord
    CCORDcode code = ccord_global_init();
    if (code) {
        log_trace("[QOTD] ccord_global_init() failed: %d", code);

        return NULL;
    }
    log_trace("[QOTD] ccord_global_init() success");

    // create discord client
    struct discord* client = discord_config_init(CONFIG_FILE);
    if (!client) {
        log_trace("[QOTD] discord_create() failed");

        ccord_global_cleanup();
        return NULL;
    }
    log_trace("[QOTD] discord_create() success");

    return client;
}

char* qotd_message = NULL; //!< Current question of the day message

/**
 * Handle setqotd interaction
 *
 * \param client Discord client
 * \param event Interaction event
 */
static void on_setqotd(struct discord *client, const struct discord_interaction *event) {
    // check if command was issued by lumi
    if (event->member->user->id != LUMI_USER_ID) {
        discord_create_interaction_response(client, event->id, event->token, &(struct discord_interaction_response) {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data) {
                .flags = DISCORD_MESSAGE_EPHEMERAL,
                .content = "Hold up! Only Lumi can update the question of the day..."
            }
        }, NULL);

        log_info("[QOTD] User %s tried to update question of the day", event->member->user->username);
        return;
    }

    // verify that the command has the correct options (should never be triggered, but just in case)
    if (event->data->options->size != 1 || event->data->options->array[0].type != DISCORD_APPLICATION_OPTION_STRING) {
        discord_create_interaction_response(client, event->id, event->token, &(struct discord_interaction_response) {
            .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data) {
                .flags = DISCORD_MESSAGE_EPHEMERAL,
                .content = "Invalid command usage, please provide a message!"
            }
        }, NULL);

        log_info("[QOTD] User %s tried to update qotd without providing a message", event->member->user->username);
        return;
    }

    // set the qotd
    qotd_message = strdup(event->data->options->array[0].value);

    // send response
    char response[4001];
    snprintf(response, sizeof(response), "Question of the day has been updated to:\n> \"%s\"", qotd_message);
    discord_create_interaction_response(client, event->id, event->token, &(struct discord_interaction_response) {
        .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data) {
            .flags = DISCORD_MESSAGE_EPHEMERAL,
            .content = response
        }
    }, NULL);

    log_info("[QOTD] User %s updated qotd to \"%s\"", event->member->user->username, qotd_message);
}

/**
 * Handle qotd timer
 *
 * \param client Discord client
 * \param timer Timer
 */
static void on_qotd_timer(struct discord* client, struct discord_timer* timer) {
    // check if qotd is set
    if (!qotd_message) {
        log_info("[QOTD] QOTD is not set, skipping...");
        return;
    }

    // send qotd
    char message[4001];
    snprintf(message, sizeof(message), "<@&%llu> %s", QOTD_ROLE_ID, qotd_message);
    discord_create_message(client, QOTD_CHANNEL_ID, &(struct discord_create_message) {
        .content = message
    }, NULL);

    // clear qotd
    free(qotd_message);
    qotd_message = NULL;
}

bool is_initialized = false; //!< Whether the bot was initialized

/**
 * Main bot function
 *
 * \param client Discord client
 * \param event Ready event
 */
void bot_main(struct discord* client, const struct discord_ready *event) {
    // check if bot was already initialized
    if (is_initialized) return;
    is_initialized = true;

    // initialize global slash commands
    log_info("[QOTD] Initializing global slash commands...");
    discord_bulk_overwrite_global_application_commands(client, event->application->id, &(struct discord_application_commands) {
        .size = 1,
        .array = &(struct discord_application_command) {
            .type = DISCORD_APPLICATION_CHAT_INPUT,
            .name = "setqotd",
            .description = "Set the next question of the day",
            .default_permission = false,
            .application_id = event->application->id,
            .options = &(struct discord_application_command_options) {
                .size = 1,
                .array = &(struct discord_application_command_option) {
                    .type = DISCORD_APPLICATION_OPTION_STRING,
                    .name = "message",
                    .description = "The text message to send as question of the day, the bot will mention the role by itself",
                    .required = true
                }
            }
        }
    }, NULL);

    // set up timer to send qotd
    time_t time_now = time(NULL);
    struct tm* timeinfo_qotd = gmtime(&time_now);
    timeinfo_qotd->tm_hour = 20;
    timeinfo_qotd->tm_min = 0;
    timeinfo_qotd->tm_sec = 10;
    time_t time_qotd = timegm(timeinfo_qotd);
    if ((time_qotd-15) < time_now) time_qotd += (24 * 60 * 60);
    int64_t seconds_until_qotd = (uint64_t) difftime(time_qotd, time_now);

    // create timer
        log_info("[QOTD] Setting up timer to send question of the day in %ld seconds", seconds_until_qotd);
    discord_timer_interval(client, on_qotd_timer, NULL, NULL, seconds_until_qotd * 1000, 24 * 60 * 60 * 1000, -1);
}

/**
 * Main function
 *
 * \return 0 on success, 1 on failure
 */
int main() {
    // initialize discord bot
    log_info("[QOTD] Initializing qotd discord bot...");
    struct discord* client = initialize_discord();
    if (!client) {
        log_fatal("[QOTD] Failed to initialize discord bot");

        return EXIT_FAILURE;
    }

    // run discord bot
    log_info("[QOTD] Launching qotd discord bot...");
    signal(SIGINT, handle_sigint);
    discord_set_on_ready(client, bot_main);
    discord_set_on_interaction_create(client, on_setqotd);
    CCORDcode code = discord_run(client);

    // cleanup discord bot
    log_info("[QOTD] Discord bot exited (%d), cleaning up...", code);
    discord_cleanup(client);
    ccord_global_cleanup();
    return EXIT_SUCCESS;
}
