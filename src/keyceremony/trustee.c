#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "keyceremony/message_reps.h"
#include "keyceremony/trustee.h"
#include "max_values.h"
#include "serialize/keyceremony.h"
#include "serialize/trustee_state.h"
#include "trustee_state_rep.h"

struct KeyCeremony_Trustee_s
{
    uint32_t num_trustees;
    uint32_t threshold;
    uint32_t index;
    //@secret the private key must not be leaked from the system
    struct private_key private_key;
    struct public_key public_keys[MAX_TRUSTEES];
};

struct KeyCeremony_Trustee_new_r KeyCeremony_Trustee_new(uint32_t num_trustees,
                                                         uint32_t threshold,
                                                         uint32_t index)
{
    struct KeyCeremony_Trustee_new_r result;
    result.status = KEYCEREMONY_TRUSTEE_SUCCESS;

    if (!(1 <= threshold && threshold <= num_trustees &&
          num_trustees <= MAX_TRUSTEES))
        result.status = KEYCEREMONY_TRUSTEE_INVALID_PARAMS;

    // Allocate the trustee
    if (result.status == KEYCEREMONY_TRUSTEE_SUCCESS)
    {
        result.trustee = malloc(sizeof(struct KeyCeremony_Trustee_s));
        if (result.trustee == NULL)
            result.status = KEYCEREMONY_TRUSTEE_INSUFFICIENT_MEMORY;
    }

    // Initialize the trustee
    if (result.status == KEYCEREMONY_TRUSTEE_SUCCESS)
    {
        result.trustee->num_trustees = num_trustees;
        result.trustee->threshold = threshold;
        result.trustee->index = index;
    }

    return result;
}

void KeyCeremony_Trustee_free(KeyCeremony_Trustee t) { free(t); }

struct KeyCeremony_Trustee_generate_key_r
KeyCeremony_Trustee_generate_key(KeyCeremony_Trustee t)
{
    struct KeyCeremony_Trustee_generate_key_r result;
    result.status = KEYCEREMONY_TRUSTEE_SUCCESS;

    // Generate the keypair
    struct Crypto_gen_keypair_r crypto_result =
        Crypto_gen_keypair(t->threshold);
    switch (crypto_result.status)
    {
    case CRYPTO_INSUFFICIENT_MEMORY:
        result.status = KEYCEREMONY_TRUSTEE_INSUFFICIENT_MEMORY;
        break;
    case CRYPTO_SUCCESS:
        break;
    default:
        //@ assert false;
        assert(false && "unreachable");
    };

    if (result.status == KEYCEREMONY_TRUSTEE_SUCCESS)
    {
        Crypto_private_key_copy(&t->private_key, &crypto_result.private_key);
        Crypto_public_key_copy(&t->public_keys[t->index],
                               &crypto_result.public_key);
    }

    if (result.status == KEYCEREMONY_TRUSTEE_SUCCESS)
    {
        // Build the message
        struct key_generated_rep message_rep;

        message_rep.trustee_index = t->index;
        Crypto_public_key_copy(&message_rep.public_key,
                               &t->public_keys[t->index]);

        // Serialize the message
        struct serialize_state state = {
            .status = SERIALIZE_STATE_RESERVING,
            .len = 0,
            .offset = 0,
            .buf = NULL,
        };

        Serialize_reserve_key_generated(&state, &message_rep);
        Serialize_allocate(&state);
        Serialize_write_key_generated(&state, &message_rep);

        if (state.status != SERIALIZE_STATE_WRITING)
            result.status = KEYCEREMONY_TRUSTEE_SERIALIZE_ERROR;
        else
        {
            result.message = (struct key_generated_message){
                .len = state.len,
                .bytes = state.buf,
            };
        }
    }

    return result;
}

struct KeyCeremony_Trustee_generate_shares_r
KeyCeremony_Trustee_generate_shares(KeyCeremony_Trustee t,
                                    struct all_keys_received_message in_message)
{
    struct KeyCeremony_Trustee_generate_shares_r result;
    result.status = KEYCEREMONY_TRUSTEE_SUCCESS;

    struct all_keys_received_rep in_message_rep;

    // Deserialize the input
    {
        struct serialize_state state = {
            .status = SERIALIZE_STATE_READING,
            .len = in_message.len,
            .offset = 0,
            .buf = (uint8_t *)in_message.bytes,
        };

        Serialize_read_all_keys_received(&state, &in_message_rep);

        if (state.status != SERIALIZE_STATE_READING)
            result.status = KEYCEREMONY_TRUSTEE_DESERIALIZE_ERROR;
    }

    // Check that my public key is present at t->index
    if (result.status == KEYCEREMONY_TRUSTEE_SUCCESS)
        if (!Crypto_public_key_equal(&in_message_rep.public_keys[t->index],
                                     &t->public_keys[t->index]))
            result.status = KEYCEREMONY_TRUSTEE_MISSING_PUBLIC_KEY;

    // Copy other public keys into my state
    if (result.status == KEYCEREMONY_TRUSTEE_SUCCESS)
        for (uint32_t i = 0; i < t->num_trustees; i++)
            Crypto_public_key_copy(&t->public_keys[i],
                                   &in_message_rep.public_keys[i]);

    if (result.status == KEYCEREMONY_TRUSTEE_SUCCESS)
    {
        // Build the message
        struct shares_generated_rep out_message_rep;

        out_message_rep.trustee_index = t->index;
        out_message_rep.num_trustees = t->num_trustees;
        for (uint32_t i = 0; i < t->num_trustees; i++)
        {
            Crypto_private_key_copy(&out_message_rep.shares[i].private_key,
                                    &t->private_key);
            Crypto_public_key_copy(
                &out_message_rep.shares[i].recipient_public_key,
                &t->public_keys[i]);
        }

        // Serialize the message
        struct serialize_state state = {
            .status = SERIALIZE_STATE_RESERVING,
            .len = 0,
            .offset = 0,
            .buf = NULL,
        };

        Serialize_reserve_shares_generated(&state, &out_message_rep);
        Serialize_allocate(&state);
        Serialize_write_shares_generated(&state, &out_message_rep);

        if (state.status != SERIALIZE_STATE_WRITING)
            result.status = KEYCEREMONY_TRUSTEE_SERIALIZE_ERROR;
        else
        {
            result.message = (struct shares_generated_message){
                .len = state.len,
                .bytes = state.buf,
            };
        }
    }

    return result;
}

struct KeyCeremony_Trustee_verify_shares_r
KeyCeremony_Trustee_verify_shares(KeyCeremony_Trustee t,
                                  struct all_shares_received_message in_message)
{
    struct KeyCeremony_Trustee_verify_shares_r result = {
        .status = KEYCEREMONY_TRUSTEE_SUCCESS,
    };

    struct all_shares_received_rep in_message_rep;

    // Deserialize the input
    {
        struct serialize_state state = {
            .status = SERIALIZE_STATE_READING,
            .len = in_message.len,
            .offset = 0,
            .buf = (uint8_t *)in_message.bytes,
        };

        Serialize_read_all_shares_received(&state, &in_message_rep);

        if (state.status != SERIALIZE_STATE_READING)
            result.status = KEYCEREMONY_TRUSTEE_DESERIALIZE_ERROR;
    }

    // Check that all the shares meant for me match the public keys
    // previously received
    for (uint32_t i = 0; i < t->threshold; i++)
    {
        struct encrypted_key_share share = in_message_rep.shares[t->index][i];

        if (!Crypto_public_key_equal(&share.recipient_public_key,
                                     &t->public_keys[i]))
            result.status = KEYCEREMONY_TRUSTEE_INVALID_KEY_SHARE;
    }

    if (result.status == KEYCEREMONY_TRUSTEE_SUCCESS)
    {
        // Build the message
        struct shares_verified_rep out_message_rep = {
            .trustee_index = t->index,
            .verified = true,
        };

        // Serialize the message
        struct serialize_state state = {
            .status = SERIALIZE_STATE_RESERVING,
            .len = 0,
            .offset = 0,
            .buf = NULL,
        };

        Serialize_reserve_shares_verified(&state, &out_message_rep);
        Serialize_allocate(&state);
        Serialize_write_shares_verified(&state, &out_message_rep);

        if (state.status != SERIALIZE_STATE_WRITING)
            result.status = KEYCEREMONY_TRUSTEE_SERIALIZE_ERROR;
        else
        {
            result.message = (struct shares_verified_message){
                .len = state.len,
                .bytes = state.buf,
            };
        }
    }

    return result;
}

struct KeyCeremony_Trustee_export_state_r
KeyCeremony_Trustee_export_state(KeyCeremony_Trustee t)
{
    struct KeyCeremony_Trustee_export_state_r result;
    result.status = KEYCEREMONY_TRUSTEE_SUCCESS;

    {
        struct trustee_state_rep rep;
        rep.index = t->index;
        Crypto_private_key_copy(&rep.private_key, &t->private_key);

        // Serialize the message
        struct serialize_state state = {
            .status = SERIALIZE_STATE_RESERVING,
            .len = 0,
            .offset = 0,
            .buf = NULL,
        };

        Serialize_reserve_trustee_state(&state, &rep);
        Serialize_allocate(&state);
        Serialize_write_trustee_state(&state, &rep);

        if (state.status != SERIALIZE_STATE_WRITING)
            result.status = KEYCEREMONY_TRUSTEE_SERIALIZE_ERROR;
        else
        {
            result.state = (struct trustee_state){
                .len = state.len,
                .bytes = state.buf,
            };
        }
    }

    return result;
}