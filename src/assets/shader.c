#include "shader.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "assets.h"
#include "../shaders/shaderinfo.h"

static struct shader* shader_load_file(const char* path, GLenum type) {

    FILE* file = fopen(path, "r");
    if(file == NULL) {
        printf("Failed loading shader file %s\n", path);
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    size_t length = ftell(file);
    fseek(file, 0, SEEK_SET);

    char* buffer = malloc(length + 1);
    if(buffer == NULL) {
        printf("Failed allocating string for shader %s of length %ld\n", path, length);
        return NULL;
    }

    if(fread(buffer, 1, length, file) != length) {
        printf("Failed reading the shader %s\n", path);
        free(buffer);
        return NULL;
    }
    buffer[length] = '\0';

    fclose(file);

    struct shader* shader = malloc(sizeof(struct shader));

    shader->gl_shader = glCreateShader(type);
    if(shader < 0) {
        printf("Failed creating the shader object for %s\n", path);
        free(buffer);
        free(shader);
        return NULL;
    }

    glShaderSource(shader->gl_shader, 1, (const char**)&buffer, NULL);
    glCompileShader(shader->gl_shader);

    // GL Lets you free the string right after the compile
    free(buffer);

    int status = GL_FALSE;
    glGetShaderiv(shader->gl_shader, GL_COMPILE_STATUS, &status);
    if(status == GL_FALSE) {
        printf("Failed compiling shader %s\n", path);

        GLint log_len = 0;
        glGetShaderiv(shader->gl_shader, GL_INFO_LOG_LENGTH, &log_len);
        if (log_len) {
            char log[log_len + 1];
            glGetShaderInfoLog(shader->gl_shader, log_len, NULL, log);
            printf(" -- %s\n", log);
            fflush(stdout);
        }

        return NULL;
    }

    return shader;
}

struct shader* vert_shader_load_file(const char* path) {
    return shader_load_file(path, GL_VERTEX_SHADER);
}

struct shader* frag_shader_load_file(const char* path) {
    return shader_load_file(path, GL_FRAGMENT_SHADER);
}


void shader_unload_file(struct shader* asset) {
    glDeleteShader(asset->gl_shader);
    free(asset);
}

static void shader_program_link(struct shader_program* program) {
    program->gl_program = glCreateProgram();
    if(program->gl_program == 0) {
        printf("Failed creating program\n");
        return;
    }

    glAttachShader(program->gl_program, program->fragment->gl_shader);
    glAttachShader(program->gl_program, program->vertex->gl_shader);

    // @FRAGILE 64 here is has to be the same as the MAXIMUM length of a shader
    // variable name
    char name[64] = {0};
    int* key;
    JSLF(key, program->attributes, name);
    while(key != NULL) {
        glBindAttribLocation(program->gl_program, *key, name);
        JSLN(key, program->attributes, name);
    }

    glLinkProgram(program->gl_program);

    GLint status = GL_FALSE;
    glGetProgramiv(program->gl_program, GL_LINK_STATUS, &status);
    if (GL_FALSE == status) {
        printf("Failed linking shader\n");

        GLint log_len = 0;
        glGetProgramiv(program->gl_program, GL_INFO_LOG_LENGTH, &log_len);
        if (log_len) {
            char log[log_len + 1];
            glGetProgramInfoLog(program->gl_program, log_len, NULL, log);
            printf("-- %s\n", log);
            fflush(stdout);
        }

        glDeleteProgram(program->gl_program);
    }
}

struct shader_program* shader_program_load_file(const char* path) {
    FILE* file = fopen(path, "r");
    if(file == NULL) {
        printf("Failed opening shader program file %s\n", path);
        return NULL;
    }

    struct shader_program* program = malloc(sizeof(struct shader_program));
    program->vertex = NULL;
    program->fragment = NULL;
    program->attributes = NULL;
    program->gl_program = -1;

    char* shader_type = NULL;

    char* line = NULL;
    size_t line_size = 0;

    size_t read;
    read = getline(&line, &line_size, file);
    if(read <= 0 || strcmp(line, "#version 1\n") != 0) {
        printf("No version found at the start of file %s\n", path);
        return NULL;
    }

    while((read = getline(&line, &line_size, file)) != -1) {
        if(read == 0 || line[0] == '#')
            continue;

        // Remove the trailing newlines
        line[strcspn(line, "\r\n")] = '\0';

        char type[64];
        char value[64];
        int matches = sscanf(line, "%63s %63[^\n]", type, value);

        // An EOF from matching means either an error or empty line. We will
        // just swallow those.
        if(matches == EOF)
            continue;

        if(matches != 2) {
            printf("Wrongly formatted line \"%s\", ignoring\n", line);
            continue;
        }

        if(strcmp(type, "vertex") == 0) {
            if(program->vertex != NULL) {
                printf("Multiple vertex shader defs in file %s, ignoring \"%s\"\n", path, line);
                continue;
            }
            program->vertex = assets_load(value);
            if(program->vertex == NULL) {
                printf("Failed loading vertex shader for %s, ignoring \"%s\"\n", path, line);
            }
        } else if(strcmp(type, "fragment") == 0) {
            if(program->fragment != NULL) {
                printf("Multiple fragment shader defs in file %s, ignoring \"%s\"\n", path, line);
                continue;
            }
            program->fragment = assets_load(value);
            if(program->fragment == NULL) {
                printf("Failed loading fragment shader for %s, ignoring \"%s\"\n", path, line);
                continue;
            }
        } else if(strcmp(type, "type") == 0) {
            if(shader_type != NULL) {
                printf("Multiple type defs in file %s, ignoring \"%s\"\n", path, line);
                continue;
            }
            shader_type = strdup(value);
            if(shader_type == NULL) {
                printf("Failed duplicating type string for %s, ignoring \"%s\"\n", path, line);
                continue;
            }
        } else if(strcmp(type, "attrib") == 0) {
            int key;
            char name[64];
            int matches = sscanf(value, "%d %63s", &key, name);

            if(matches != 2) {
                printf("Couldn't parse the attrib definition \"%s\"\n", value);
                continue;
            }

            int* index;
            JSLG(index, program->attributes, name);
            if(index != NULL) {
                printf("Attrib name %d redefine ignored\n", name);
                continue;
            }

            JSLI(index, program->attributes, name);
            if(index == NULL) {
                printf("Failed inserting %s into the attribute array, ignoring\n", name);
            }
            *index = key;
        } else {
            printf("Unknown directive \"%s\" in shader file %s, ignoring\n", line, path);
        }
    }

    free(line);
    fclose(file);

    if(program->vertex == NULL) {
        printf("Vertex shader not set in %s\n", path);
        // @LEAK We might be leaking the fragment shader, but the assets manager
        // still has a hold of it
        if(shader_type != NULL)
            free(shader_type);
        free(program);
        return NULL;
    }
    if(program->fragment == NULL) {
        printf("Fragment shader not set in %s\n", path);
        // @LEAK We might be leaking the vertex shader, but the assets manager
        // still has a hold of it
        if(shader_type != NULL)
            free(shader_type);
        free(program);
        return NULL;
    }
    if(shader_type == NULL) {
        printf("Type not set in %s\n", path);
        // @LEAK We might be leaking the vertex shader, but the assets manager
        // still has a hold of it
        free(shader_type);
        free(program);
        return NULL;
    }

    shader_program_link(program);

    struct shader_type_info* shader_info = get_shader_type_info(shader_type);
    if(shader_info == NULL) {
        printf("Failed to find shader type info for %s\n", shader_type);
        free(shader_type);
        glDeleteProgram(program->gl_program);
        free(program);
        return NULL;
    }

    program->shader_type_info = shader_info;

    program->shader_type = malloc(shader_info->size);
    if(program->shader_type == NULL) {
        printf("Failed to allocate space for the shader type\n");
        free(shader_type);
        glDeleteProgram(program->gl_program);
        free(program);
        return NULL;
    }

    for(int i = 0; i < shader_info->member_count; i++) {
        struct shader_uniform_info* uniform_info = &shader_info->members[i];
        GLint* field = (GLint*)(program->shader_type + uniform_info->offset);
        *field = glGetUniformLocation(program->gl_program, uniform_info->name);
    }


    return program;
}

void shader_program_unload_file(struct shader_program* asset) {
    glDeleteProgram(asset->gl_program);
    free(asset->shader_type);
    Word_t freed;
    JSLFA(freed, asset->attributes);
    free(asset);
}